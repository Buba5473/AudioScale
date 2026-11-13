/*
   scale — общий движок.

   Copyright (c) 2004, by Zhuo Meng (zhuo@thunder.cwru.edu).
   Distributed under GNU GPL v2 or later.

   Здесь реализованы:
     - разбор аргументов (-t: сек / кадры / hh:mm:ss) и печать времени;
     - детектор формата (DTS → AC-3 → MP2);
     - bounds-safe поиск кадров (без выхода за буфер);
     - параллельный head-scan (std::async);
     - покадровый проход с codec.frame_span() — корректно работает с
       MP2 padding и DTS-HD MA/HRA (автоматически отбрасывает extension);
     - построение плана дублирования/удаления и его применение в
       конвейере reader/writer (std::jthread + BoundedQueue).
*/

#include "scale/common.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Переносимые 64-битные fseek/ftell (Windows long = 32 бита даже на x64).
// ---------------------------------------------------------------------------
#ifdef _WIN32
#  define SCALE_FSEEK(f, off, whence) ::_fseeki64((f), (off), (whence))
#else
#  define SCALE_FSEEK(f, off, whence) \
    std::fseek((f), static_cast<long>(off), (whence))
#endif

namespace scale {

#ifdef SCALE_VERSION
static_assert(kVersion == std::string_view{SCALE_VERSION},
              "kVersion in include/scale/common.hpp must match "
              "project(... VERSION ...) in CMakeLists.txt");
#endif

namespace {

constexpr std::size_t kScanBuf  = 1u << 20;   // 1 MiB — окно поиска sync
// Размер окна детекции совпадает с kBufSize: должен покрывать
// любой разумный ведущий мусор, иначе detect_codec() не найдёт
// первый кадр в слишком коротком head-буфере.
// Размер kScanBuf должен быть не меньше максимального кадра DTS Core
// (16384 байт) плюс запас на ведущий мусор.
constexpr std::size_t kQueueCap = 64;         // глубина конвейера
// Защитный верхний предел для find_frame(); каждый кодек
// имеет собственный меньший предел (MP2 ≈ 1730, AC-3 3840,
// DTS 16384), но этот запас используется в static_assert ниже.
constexpr std::size_t kMaxFrame = 65536;
// Буфер обязан вмещать кадр DTS Core вместе с XLL/X96-расширением,
// иначе frame_span() не найдёт следующий sync и обработка прервётся.
// Верхняя граница DTS-HD MA ~24.5 Мбит/с при длительности кадра
// ~85 мс даёт около 260 КБ; берём 1 МиБ с запасом.
constexpr std::size_t kBufSize  = 1u << 20;
using scale::detail::kNone;  // single source of truth: include/scale/common.hpp

// Буфер обязан вмещать кадр DTS-HD MA целиком. Оценка верхней
// границы: 24.5 Мбит/с × ~85 мс ≈ 260 КБ; 1 МиБ даёт запас ~4x.
static_assert(kBufSize >= 300u * 1024u,
              "kBufSize слишком мал для кадра DTS-HD MA");
static_assert(kBufSize > kMaxFrame,
              "kBufSize должен вмещать хотя бы один максимальный кадр");
static_assert(kScanBuf >= 16384 + 1024,
              "kScanBuf must fit max DTS Core frame (16384) + margin");

using FilePtr = std::unique_ptr<std::FILE, int(*)(std::FILE*)>;

// ---------------------------------------------------------------------------
// Печать usage
// ---------------------------------------------------------------------------
void print_usage(std::FILE* out, std::string_view prg) {
    print(out,
        "{0} {1} — audio stream length scaler\n"
        "\n"
        "Usage: {0} [-t target] [-o outfile] infile\n"
        "       {0} -h | --help\n"
        "       {0} -V | --version\n"
        "\n"
        "Scales MP2 / AC-3 / DTS Core audio to the target length by\n"
        "duplicating or dropping whole frames. When infile is DTS-HD\n"
        "MA/HRA, the DTS Core is extracted automatically.\n"
        "\n"
        "  -t target   Target length. Accepts:\n"
        "                 20.5        20.5 seconds (number)\n"
        "                 20.5s       20.5 seconds (explicit)\n"
        "                 500f        500 frames\n"
        "                 00:20:30    20 min 30 sec (hh:mm:ss)\n"
        "                 01:30       1 min 30 sec (mm:ss)\n"
        "              Without -t only reports stream properties.\n"
        "  -o outfile  Write scaled stream to outfile.\n"
        "  -h, --help  Show this message.\n"
        "  -V, --version  Show version and exit.\n"
"  --             End of options; remaining args are "
"treated as paths.\n", prg, kVersion);
#ifdef _WIN32
    print(out, "  /h, /?         Windows-style help (cmd.exe).\n");
#endif
}

// ---------------------------------------------------------------------------
// Ограниченная очередь producer/consumer с корректным завершением.
//   push   возвращает false, если очередь закрыта;
//   pop    возвращает nullopt, если очередь пуста И закрыта.
// ---------------------------------------------------------------------------
template <class T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t cap) : cap_(cap) {}
    BoundedQueue(const BoundedQueue&)            = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    bool push(T v) {
        std::unique_lock lk(m_);
        cv_not_full_.wait(lk, [&] { return q_.size() < cap_ || closed_; });
        if (closed_) return false;
        q_.push(std::move(v));
        cv_not_empty_.notify_one();
        return true;
    }

    std::optional<T> pop() {
        std::unique_lock lk(m_);
        cv_not_empty_.wait(lk, [&] { return !q_.empty() || closed_; });
        if (q_.empty()) return std::nullopt;
        T v = std::move(q_.front());
        q_.pop();
        cv_not_full_.notify_one();
        return v;
    }

    void close() {
        std::lock_guard lk(m_);
        closed_ = true;
        cv_not_empty_.notify_all();
        cv_not_full_.notify_all();
    }

private:
    std::size_t             cap_;
    std::queue<T>           q_;
    std::mutex              m_;
    std::condition_variable cv_not_empty_, cv_not_full_;
    bool                    closed_ = false;
};

// Кадр, передаваемый по конвейеру. Хранит только ядро (без DTS-HD
// extension) — движок всегда пишет DTS Core.
struct FrameBlock {
    std::vector<std::uint8_t> bytes;
    double                    framesec = 0.0;
};

// Действие над кадром в плане масштабирования.
enum class Action : std::uint8_t { Copy, Drop, Dup };

// Распределить ровно `count` действий `action` по `nframes` позициям
// Bresenham-подобным алгоритмом (равномерно, без скоплений).
// Используется как для дублирования, так и для удаления кадров.
//
// При count >= nframes каждая позиция получит действие (все Dup
// либо все Drop) — это используется вызывающим кодом для обработки
// недостижимой цели.
void distribute_actions(std::vector<Action>& plan,
                        std::size_t nframes,
                        long count,
                        Action action) {
    if (count <= 0 || nframes == 0) return;
    const long total = static_cast<long>(nframes);
    long acc = 0;
    for (std::size_t i = 0; i < nframes; ++i) {
        acc += count;
        if (acc >= total) {
            acc -= total;
            plan[i] = action;
        }
    }
}

// ---------------------------------------------------------------------------
// Разбор double с полной проверкой.
// ---------------------------------------------------------------------------
bool parse_double(std::string_view s, double& out) {
    if (s.empty()) return false;
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
    // Локаль-независимый парсер с плавающей точкой.
    const char* first = s.data();
    const char* last  = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(first, last, out);
    if (ec != std::errc{} || ptr != last) return false;
#else
    try {
        std::size_t pos = 0;
        out = std::stod(std::string(s), &pos);
        if (pos != s.size()) return false;
    } catch (...) {
        return false;
    }
#endif
    return std::isfinite(out) && out >= 0.0;
}

// ---------------------------------------------------------------------------
// Bounds-safe поиск первого полного кадра в буфере.
//
//   total_size           — полный размер входного файла (0 = buf.size());
//   allow_trailing_junk  — разрешить одиночный кадр в начале потока,
//                          за которым нет валидного sync (хвостовой
//                          мусор). Применяется только на relax-проходе
//                          detect_codec(), чтобы не перекрывать строгое
//                          совпадение другого кодека.
// ---------------------------------------------------------------------------
std::optional<std::pair<std::size_t, FrameInfo>>
find_frame(std::span<const std::uint8_t> buf, const Codec& c,
           std::size_t total_size = 0,
           bool allow_trailing_junk = false) {
    const std::size_t n = buf.size();
    if (n < 10) return std::nullopt;
    if (total_size == 0) total_size = n;

    for (std::size_t i = 0; i + 10 <= n; ++i) {
        auto view = buf.subspan(i);
        if (!c.has_sync(view)) continue;

        auto fi = c.parse_header(view);
        if (!fi) continue;

        const std::size_t core = fi->framebytes;
        if (core > kMaxFrame) continue;             // защитный верхний предел
        if (i + core > total_size) continue;        // кадр не влезает в файл

        // Для MP2 / AC-3 сразу за ядром должен идти следующий sync —
        // это отсеивает случайные совпадения 0xFFF / 0x0B77 в мусоре.
        // Для DTS после ядра может быть XLL/HRA extension, поэтому
        // проверку не делаем (детектор всё равно попробует DTS первым).
        if (!c.has_extension()) {
            // Кадр заканчивается ровно в конце файла — одиночный кадр.
            if (i + core == total_size) return std::pair{i, *fi};
            // Иначе проверяем следующий sync, если он влезает в буфер.
            if (i + core <= n) {
                auto next = buf.subspan(i + core);
                if (c.has_sync(next) && c.parse_header(next))
                    return std::pair{i, *fi};
            }
            // Хвостовой мусор после одиночного кадра: принимаем,
            // только если кадр идёт с самого начала и это relax-проход.
            // Послабление позволяет детектировать поток из одного кадра
            // с «прицепом», не открывая дорогу ложным срабатываниям
            // sync-слов в середине мусорного файла.
            if (allow_trailing_junk && i == 0)
                return std::pair{i, *fi};
            continue;
        }

        return std::pair{i, *fi};
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Проверяет, начинается ли DTS-HD extension (XLL/X96) по указанному
// смещению в буфере. Возвращает false, если данных недостаточно.
[[nodiscard]] bool has_dts_hd_extension(std::span<const std::uint8_t> buf,
                                        std::size_t offset) noexcept {
    if (offset > buf.size() || buf.size() - offset < 4) return false;
    const std::uint32_t sig = detail::read_be32(buf.subspan(offset, 4));
    // DTS-HD MA: 0x41A29547; DTS-HD HRA: 0x1D95F262.
    return sig == 0x41A29547u || sig == 0x1D95F262u;
}

// ---------------------------------------------------------------------------
// Сколько байт занимает последний кадр (когда frame_span() == kNone).
//
//   frame_buf  — данные от sync текущего кадра до конца доступного буфера;
//   core       — размер ядра (fi.framebytes);
//   byte_limit — максимум байт, которые можно взять. Для конвейера это
//                remaining в файле; для tail-сканирования — весь остаток
//                буфера (frame_buf.size()).
//
// Если сразу за ядром идёт сигнатура DTS-HD extension (XLL/X96) — берём
// весь доступный остаток (ограниченный byte_limit). Иначе — только ядро.
// has_dts_hd_extension() сам отсекает случай core+4 > buf.size(),
// поэтому чтения за границей буфера здесь нет.
// ---------------------------------------------------------------------------
[[nodiscard]] std::size_t
last_frame_extent(std::span<const std::uint8_t> frame_buf,
                  std::size_t core,
                  std::size_t byte_limit) noexcept {
    if (has_dts_hd_extension(frame_buf, core))
        return std::min(byte_limit, frame_buf.size());
    return core;
}

// ---------------------------------------------------------------------------
// Размер хвостового мусора в окне tail: сколько байт в конце не
// принадлежат последнему полному кадру.
// ---------------------------------------------------------------------------
std::size_t tail_junk_size(std::span<const std::uint8_t> tail, const Codec& c) {
    std::size_t last_end = 0;

    for (std::size_t i = 0; i + 10 <= tail.size(); ++i) {
        if (!c.has_sync(tail.subspan(i))) continue;
        auto fi = c.parse_header(tail.subspan(i));
        if (!fi) continue;
        if (i + fi->framebytes > tail.size()) continue;

        const std::size_t span = c.frame_span(tail.subspan(i), *fi);

        std::size_t end;
        if (span == kNone || i + span > tail.size()) {
            // frame_span не нашёл следующий sync — это последний кадр.
            // last_frame_extent() решает, брать только ядро или
            // ядро+extension (DTS-HD MA/HRA).
            const auto frame_view = tail.subspan(i);
            end = i + last_frame_extent(frame_view, fi->framebytes,
                                        frame_view.size());
        } else {
            end = i + span;
        }
        if (end > last_end) last_end = end;
    }

    // Если в окне нет ни одного кадра — весь хвост является мусором.
    if (last_end == 0) return tail.size();
    return tail.size() - last_end;
}

// ---------------------------------------------------------------------------
// Результат обхода потока кадр за кадром.
// ---------------------------------------------------------------------------
struct ScanResult {
    std::size_t nframes   = 0;
    double      total_sec = 0.0;
    bool        ok        = false;
};

// ---------------------------------------------------------------------------
// Статус обхода:
//   Ok    — поток полностью обработан (остаток len равен нулю);
//   Eof   — достигнут конец файла либо callback попросил остановиться;
//   Error — ошибка ввода-вывода или невалидный кадр.
// ---------------------------------------------------------------------------
enum class ReadStatus { Ok, Eof, Error };

// ---------------------------------------------------------------------------
// Универсальный обход потока кадр за кадром.
//
// Один и тот же цикл используется и для подсчёта кадров (scan_frames),
// и для конвейера reader/writer в scale_stream(), что исключает
// дублирование логики обработки kNone / переполнения буфера.
//
// Callback получает span ровно на core байт (само ядро кадра без
// DTS-HD extension) и FrameInfo. Возврат true — продолжать, false —
// прекратить обход (эквивалентно ReadStatus::Eof).
// ---------------------------------------------------------------------------
template <class Fn>
ReadStatus for_each_frame(std::FILE* fin, std::size_t start, std::size_t len,
                          const Codec& codec, Fn&& cb,
                          std::stop_token st = {}) {
    if (SCALE_FSEEK(fin, static_cast<std::int64_t>(start), SEEK_SET) != 0)
        return ReadStatus::Error;

    std::vector<std::uint8_t> buf(kBufSize);
    std::size_t have = 0, pos = 0, remaining = len;
    bool eof_reached = false;

    auto refill = [&]() -> ReadStatus {
        std::memmove(buf.data(), buf.data() + pos, have - pos);
        have -= pos;
        pos   = 0;
        const std::size_t requested = buf.size() - have;
        const std::size_t got = std::fread(buf.data() + have, 1,
                                           requested, fin);
        if (std::ferror(fin)) return ReadStatus::Error;
        if (got < requested) eof_reached = true;
        have += got;
        return ReadStatus::Ok;
    };

    while (remaining > 0) {
        if (st.stop_requested()) return ReadStatus::Eof;

        if (pos == have) {
            have = std::fread(buf.data(), 1, buf.size(), fin);
            pos  = 0;
            if (std::ferror(fin)) return ReadStatus::Error;
            if (have < buf.size()) eof_reached = true;
            if (have == 0) return ReadStatus::Eof;
        }

        // Ensure at least kMaxHeaderBytes (10 for DTS) bytes are
        // available before header parse. Otherwise a frame boundary
        // near the buffer end causes a spurious BadFormat error.
        constexpr std::size_t kMaxHeaderBytes = 10;
        while (!eof_reached && (have - pos) < kMaxHeaderBytes) {
            const ReadStatus rs = refill();
            if (rs == ReadStatus::Error) return rs;
        }

        auto sp = std::span<const std::uint8_t>{buf.data() + pos, have - pos};
        auto fi = codec.parse_header(sp);
        if (!fi) return ReadStatus::Error;

        const std::size_t core = fi->framebytes;
        std::size_t span = codec.frame_span(sp, *fi);

        if (span == kNone || pos + span > have) {
            // Пытаемся дочитать данные, только если файл ещё не
            // закончился. После EOF это как раз тот случай, когда
            // последний кадр DTS остаётся в буфере без sync.
            if (!eof_reached && (pos > 0 || have < buf.size())) {
                const ReadStatus rs = refill();
                if (rs == ReadStatus::Error) return rs;
                continue;
            }
            if (span == kNone) {
                if (!eof_reached) {
                    // Buffer full but frame doesn't fit: exceeds kBufSize.
                    // Refuse rather than silently truncating data.
                    return ReadStatus::Error;
                }
                // Последний кадр в файле. last_frame_extent()
                // решает, брать только ядро или ядро+extension
                // (DTS-HD MA/HRA).
                const auto frame_view = std::span<const std::uint8_t>{
                    buf.data() + pos, have - pos};
                span = last_frame_extent(frame_view, core, remaining);
            } else if (pos + span > have) {
                return ReadStatus::Error;
            }
        }

        if (core > span || span > remaining) return ReadStatus::Error;
        if (pos + core > have) return ReadStatus::Error;

        auto frame = std::span<const std::uint8_t>{buf.data() + pos, core};
        if (!cb(frame, *fi)) return ReadStatus::Eof;

        pos       += span;
        remaining -= span;
    }
    return ReadStatus::Ok;
}

// ---------------------------------------------------------------------------
// Тонкая обёртка над for_each_frame(): подсчёт кадров и их суммарной
// длительности. Используется перед построением плана dup/drop.
// ---------------------------------------------------------------------------
ScanResult scan_frames(std::FILE* fin, std::size_t start, std::size_t len,
                       const Codec& codec) {
    ScanResult r;
    const ReadStatus st = for_each_frame(fin, start, len, codec,
        [&](std::span<const std::uint8_t>, const FrameInfo& fi) {
            ++r.nframes;
            r.total_sec += fi.framesec;
            return true;
        });
    r.ok = (st == ReadStatus::Ok);
    return r;
}

} // namespace

// ---------------------------------------------------------------------------
// Публичные функции
// ---------------------------------------------------------------------------

std::string_view to_string(Error e) noexcept {
    switch (e) {
        case Error::Help:        return "help requested";
        case Error::Version:     return "version requested";
        case Error::InvalidArgs: return "invalid arguments";
        case Error::FileOpen:    return "cannot open file";
        case Error::FileRead:    return "read error";
        case Error::FileWrite:   return "write error";
        case Error::FileSeek:    return "seek error";
        case Error::BadFormat:   return "unrecognized audio format";
        case Error::TooShort:    return "file is too short";
    }
    return "unknown error";
}

// ---------------------------------------------------------------------------
// Парсинг -t:
//   "20.5"     → 20.5 s
//   "20.5s"    → 20.5 s
//   "500f"     → 500 frames
//   "01:30"    → 1 min 30 sec
//   "00:20:30" → 20 min 30 sec
// ---------------------------------------------------------------------------
std::optional<TargetSpec> parse_target(std::string_view s) noexcept {
    if (s.empty()) return std::nullopt;

    // --- форма hh:mm:ss или mm:ss ---
    if (s.find(':') != std::string_view::npos) {
        // Фиксированный массив вместо std::vector: parse_target()
        // помечен noexcept, а push_back в vector может бросить
        // bad_alloc → std::terminate(). Максимум 3 компоненты
        // (hh:mm:ss); 4-я — невалидный ввод.
        std::array<std::string_view, 3> parts{};
        std::size_t nparts = 0;
        std::size_t start  = 0;
        while (start <= s.size()) {
            if (nparts >= parts.size()) return std::nullopt;  // 4-я компонента
            auto colon = s.find(':', start);
            auto end   = (colon == std::string_view::npos) ? s.size() : colon;
            parts[nparts++] = s.substr(start, end - start);
            if (colon == std::string_view::npos) break;
            start = colon + 1;
        }
        if (nparts < 2) return std::nullopt;

        static constexpr double kScale[3] = { 3600.0, 60.0, 1.0 };
        const std::size_t off = 3 - nparts;

        double total = 0.0;
        for (std::size_t i = 0; i < nparts; ++i) {
            double v = 0.0;
            if (!parse_double(parts[i], v)) return std::nullopt;
            total += v * kScale[off + i];
        }
        if (total <= 0.0) return std::nullopt;

        TargetSpec t;
        t.kind  = TargetSpec::Kind::Seconds;
        t.value = total;
        return t;
    }

    // --- суффикс 's' / 'S' → секунды ---
    const char last = s.back();
    if (last == 's' || last == 'S') {
        double v = 0.0;
        if (!parse_double(s.substr(0, s.size() - 1), v)) return std::nullopt;
        if (v <= 0.0) return std::nullopt;
        TargetSpec t;
        t.kind  = TargetSpec::Kind::Seconds;
        t.value = v;
        return t;
    }

    // --- суффикс 'f' / 'F' → кадры ---
    if (last == 'f' || last == 'F') {
        double v = 0.0;
        if (!parse_double(s.substr(0, s.size() - 1), v)) return std::nullopt;
        if (v <= 0.0) return std::nullopt;
        // Дробное число кадров не имеет физического смысла:
        // движок вставляет/удаляет целые кадры.
        if (v != std::floor(v)) return std::nullopt;
        TargetSpec t;
        t.kind  = TargetSpec::Kind::Frames;
        t.value = v;
        return t;
    }

    // --- чистое число → секунды ---
    double v = 0.0;
    if (!parse_double(s, v)) return std::nullopt;
    if (v <= 0.0) return std::nullopt;
    TargetSpec t;
    t.kind  = TargetSpec::Kind::Seconds;
    t.value = v;
    return t;
}

// ---------------------------------------------------------------------------
// Разбор аргументов командной строки.
// ---------------------------------------------------------------------------
std::expected<Args, Error>
parse_args(int argc, char** argv, std::string_view prg) {
    Args a;

    // Единые точки выхода для -h и -V: используются и в ранней
    // проверке argv, и в switch ниже (для слитных форм -hXXX/-VXXX).
    auto emit_help = [&]() -> std::expected<Args, Error> {
        print_usage(stdout, prg);
        return std::unexpected(Error::Help);
    };
    auto emit_version = [&]() -> std::expected<Args, Error> {
        print("{} {}\n", prg, kVersion);
        return std::unexpected(Error::Version);
    };

    // Единая точка выхода для «неверных аргументов»: печатает
    // опциональное сообщение, usage и возвращает InvalidArgs.
    // Устраняет дублирование print_usage(stderr, prg) + return
    // по всему телу parse_args().
    auto fail_usage = [&](std::string_view msg = {})
            -> std::expected<Args, Error> {
        if (!msg.empty()) println(stderr, "{}", msg);
        print_usage(stderr, prg);
        return std::unexpected(Error::InvalidArgs);
    };

    if (argc < 2) return fail_usage();

    bool have_infile = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view v = argv[i];

        // "--" завершает список опций: все последующие аргументы —
        // позиционные (пути), даже если начинаются с '-'.
        if (v == "--") {
            for (++i; i < argc; ++i) {
                if (have_infile)
                    return fail_usage("Multiple input files are not supported.");
                a.infile    = argv[i];
                have_infile = true;
            }
            break;
        }

        if (v == "-h" || v == "--help" || v == "/?"
#ifdef _WIN32
            || v == "/h"
#endif
            ) {
            return emit_help();
        }

        // -V / --version: печатаем версию в stdout и выходим.
        // Обработка перенесена из main.cpp сюда, чтобы сторонние
        // вызывающие parse_args() получили единообразное поведение.
        if (v == "-V" || v == "--version") {
            return emit_version();
        }

        // Определяем, опция это или путь. На Unix '/' — начало абсолютного
        // пути, а не префикс опции; учитываем это.
        if (v == "-")
            return fail_usage("Reading from stdin ('-') is not supported.");

        bool is_opt = (!v.empty() && v[0] == '-');
#ifdef _WIN32
        // На Windows исторически опции могли начинаться с '/' (cmd.exe),
        // но в MSYS2 путь тоже начинается с '/'. Поэтому распознаём '/'
        // как префикс опции только для известных форм: '/h', '/?',
        // '/t', '/o' и их слитных вариантов ('/t20.5'). Пути вида
        // '/tmp/...' не должны интерпретироваться как опции.
        if (!is_opt && v.size() >= 2 && v[0] == '/' && v[1] != '/') {
            const char c = v[1];
            if (c == '?') {
                is_opt = true;
            } else if (c == 'h') {
                is_opt = (v.size() == 2);
            } else if (c == 't') {
                is_opt = (v.size() == 2)
                      || (v.size() > 2
                          && ((v[2] >= '0' && v[2] <= '9') || v[2] == '.'));
            } else if (c == 'o') {
                is_opt = (v.size() == 2);
            }
        }
#endif

        if (is_opt) {
            // Опция может быть слитной: -t20.5, -oout.mp2.
            const char opt = v[1];  // v.size() >= 2 — is_opt check above guarantees it
            const std::string_view optarg =
                v.size() > 2 ? v.substr(2) : std::string_view{};

            // Возвращает значение опции: либо уже слитое,
            // либо следующий argv-элемент.
            auto need_value = [&](std::string_view& out) -> bool {
                if (!optarg.empty()) { out = optarg; return true; }
                if (i + 1 >= argc) return false;
                out = argv[++i];
                return true;
            };

            switch (opt) {
            // Точные формы -h/--help/-V/--version перехвачены выше,
            // до определения is_opt. Эти case-ветки достижимы только
            // для слитных форм (-hXXX, -VXXX): у флагов нет аргументов,
            // поэтому хвост optarg игнорируется.
            case 'h':
                return emit_help();
            case 'V':
                return emit_version();
            case 't': {
                std::string_view tval;
                if (!need_value(tval))
                    return fail_usage("Option -t requires a value.");
                auto spec = parse_target(tval);
                if (!spec)
                    return fail_usage(std::format("Invalid -t value: {}", tval));
                a.target = *spec;
                break;
            }
            case 'o': {
                std::string_view oval;
                if (!need_value(oval))
                    return fail_usage("Option -o requires a value.");
                a.outfile.assign(oval);
                break;
            }
            default:
                return fail_usage();
            }
        } else {
            if (have_infile)
                return fail_usage("Multiple input files are not supported.");
            a.infile    = argv[i];
            have_infile = true;
        }
    }

    if (!have_infile) return fail_usage();
    return a;
}

// ---------------------------------------------------------------------------
// Печать времени HH:MM:SS.mmm
// ---------------------------------------------------------------------------
void print_time(double t) {
    if (!(t > 0.0)) t = 0.0;
    // Округляем до миллисекунд ДО разбиения на компоненты,
    // чтобы избежать ситуации 59.9995 → "60.000" в младшем
    // поле при неизменных минутах/часах.
    const long long total_ms =
        static_cast<long long>(t * 1000.0 + 0.5);
    const long long h  = total_ms / 3600000LL;
    const long long m  = (total_ms / 60000LL) % 60LL;
    const long long s  = (total_ms / 1000LL) % 60LL;
    const long long ms = total_ms % 1000LL;
    print("{:02}:{:02}:{:02}.{:03}", h, m, s, ms);
}

// ---------------------------------------------------------------------------
// Детектор формата: DTS → AC-3 → MP2. DTS пробуется первым, потому что
// 0x7FFE8001 — самое длинное синхрослово и случайное совпадение маловероятно.
// ---------------------------------------------------------------------------
std::optional<Detected> detect_codec(std::span<const std::uint8_t> head,
                                     std::size_t total_size) noexcept {
    // FUTURE: добавить фабрики E-AC-3 / TrueHD / Opus-in-Ogg
    // в order[] и расширить размер массива. DTS должен
    // оставаться первым (самое длинное синхрослово).
    const Codec* order[3] = { &dts_codec(), &ac3_codec(), &mp2_codec() };

    // Pass 1: strict — следующий sync обязателен.
    for (const Codec* c : order) {
        auto r = find_frame(head, *c, total_size, false);
        if (r) {
            Detected d;
            d.codec  = c;
            d.offset = r->first;
            d.fi     = r->second;
            return d;
        }
    }

    // Pass 2: relax — одиночный кадр в начале + хвостовой мусор.
    for (const Codec* c : order) {
        auto r = find_frame(head, *c, total_size, true);
        if (r) {
            Detected d;
            d.codec  = c;
            d.offset = r->first;
            d.fi     = r->second;
            return d;
        }
    }

    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Полный цикл: детект, анализ, план, конвейер.
// ---------------------------------------------------------------------------
std::expected<void, Error>
scale_stream(const Args& args) {
    // -------- входной файл --------
    FilePtr fin(std::fopen(args.infile.c_str(), "rb"), &std::fclose);
    if (!fin) {
        println(stderr, "File {} could not be opened", args.infile);
        return std::unexpected(Error::FileOpen);
    }

    // -------- размер файла --------
    std::error_code ec;
    const auto fsz_opt = std::filesystem::file_size(args.infile, ec);
    if (ec) return std::unexpected(Error::FileSeek);
    const std::size_t fsize = static_cast<std::size_t>(fsz_opt);

    // 10 байт — минимальный размер, при котором detect_codec()
    // может вообще что-то проверить (DTS-заголовок = 10 байт,
    // AC-3 = 8, MP2 = 4). Всё, что меньше, гарантированно не
    // является потоком — TooShort точнее, чем BadFormat.
    if (fsize < 10) {
        println(stderr, "File {} is too short.", args.infile);
        return std::unexpected(Error::TooShort);
    }
    std::rewind(fin.get());

    // -------- 1. Детект формата --------
    const std::size_t head_len = std::min<std::size_t>(fsize, kScanBuf);
    std::vector<std::uint8_t> head(head_len);
    if (std::fread(head.data(), 1, head_len, fin.get()) != head_len)
        return std::unexpected(Error::FileRead);

    auto det = detect_codec(std::span<const std::uint8_t>{head}, fsize);
    if (!det) {
        print(stderr,
              "File {} is not a recognized audio stream. "
              "Supported codecs: {}, {}, {}.\n",
              args.infile,
              mp2_codec().name(), ac3_codec().name(), dts_codec().name());
        return std::unexpected(Error::BadFormat);
    }

    const Codec&    codec  = *det->codec;
    const std::size_t njunk = det->offset;
    const FrameInfo fi0     = det->fi;

    codec.print_info(std::span<const std::uint8_t>{head}.subspan(njunk), fi0);

    // -------- 2. Хвостовой мусор --------
    const std::size_t tail_len = std::min<std::size_t>(fsize, kBufSize);
    std::vector<std::uint8_t> tail(tail_len);
    if (SCALE_FSEEK(fin.get(),
                    static_cast<std::int64_t>(fsize - tail_len),
                    SEEK_SET) != 0)
        return std::unexpected(Error::FileSeek);
    if (std::fread(tail.data(), 1, tail_len, fin.get()) != tail_len)
        return std::unexpected(Error::FileRead);

    const std::size_t ntrail =
        tail_junk_size(std::span<const std::uint8_t>{tail}, codec);

    if (fsize < njunk + ntrail) return std::unexpected(Error::TooShort);
    const std::size_t flen = fsize - njunk - ntrail;

    // -------- 3. Точный проход по потоку --------
    ScanResult scan = scan_frames(fin.get(), njunk, flen, codec);
    if (!scan.ok) {
        print(stderr,
              "Failed to scan input stream: stream ends prematurely.\n");
        return std::unexpected(Error::FileRead);
    }
    if (scan.nframes == 0) {
        println(stderr, "No valid frames found in input stream.");
        return std::unexpected(Error::BadFormat);
    }

    const std::size_t nframes = scan.nframes;
    const double      fsec    = scan.total_sec;
    // nframes > 0 and each framesec > 0 are guaranteed by
    // parse_header() for all supported codecs.
    const double      avg_sec = fsec / static_cast<double>(nframes);

    print("Found {} leading and {} trailing junk bytes.\n", njunk, ntrail);
    print("Original length of audio is {} seconds, or ", fsec);
    print_time(fsec);
    print(", in {} frames\n", nframes);

    // -------- 4. Целевая длительность и план --------
    double target_sec = 0.0;
    if (args.target.active()) {
        target_sec = (args.target.kind == TargetSpec::Kind::Frames)
            ? args.target.value * avg_sec
            : args.target.value;
    }

    // delta — целое число кадров, которое нужно вставить (delta > 0)
    // или удалить (delta < 0). Округление до целого гарантирует,
    // что итоговое число действий совпадёт с расчётным.
    long delta = 0;
    if (target_sec > 0.0 && nframes > 0 && avg_sec > 0.0) {
        delta = std::lround((target_sec - fsec) / avg_sec);
        print("Approximately {} frames need to be ", std::labs(delta));
        if (delta > 0)      print("inserted.\n");
        else if (delta < 0) print("dropped.\n");
        else                print("nothing to do.\n");
    }

    if (args.outfile.empty()) return {};   // только отчёт

    if (!args.target.active()) {
        const char* note =
            (njunk > 0 || ntrail > 0)
                ? "leading/trailing junk will be stripped"
            : codec.has_extension()
                ? "output is DTS Core (HD extensions, if present, "
                  "will be dropped)"
                : "output will be an exact copy";
        print(stderr, "Note: -o without -t; {}.\n", note);
    }

    // -------- 5. Построение плана и открытие выхода --------
    // Отказываемся перезаписывать входной файл: fopen("wb")
    // усечёт его до того, как мы закончим чтение.
    {
        std::error_code eqec;
                bool refuse = false;
        if (std::filesystem::exists(args.outfile, eqec) && !eqec) {
            const bool eq = std::filesystem::equivalent(
                args.infile, args.outfile, eqec);
            refuse = (!eqec) ? eq : true;  // error -> refuse
        }
        if (refuse) {
            print(stderr,
                  "Refusing to overwrite input file {} with output.\n",
                  args.outfile);
            return std::unexpected(Error::FileOpen);
        }
    }

    // Сначала строим план. Если построение упадёт (std::bad_alloc),
    // выходной файл ещё не будет усечён.
    std::vector<Action> plan(nframes, Action::Copy);
    if (delta != 0 && nframes > 0) {
        const long absd = std::labs(delta);
        if (absd >= static_cast<long>(nframes)) {
            // Target unattainable in one pass.
            if (delta < 0) {
                // Dropping all frames yields an empty (0-frame) file.
                // Keep at least one frame; distribute drops evenly.
                const long keep = static_cast<long>(nframes) - 1;
                print(stderr,
                      "Warning: target requires {} drops but only {} frames "
                      "are available; dropping {} (keeping at least one "
                      "frame); result will be approximate.\n",
                      absd, nframes, keep);
                distribute_actions(plan, nframes, keep, Action::Drop);
            } else {
                print(stderr,
                      "Warning: target requires {} insertions but only {} "
                      "frames are available; inserting one duplicate per "
                      "frame; result will be approximate.\n",
                      absd, nframes);
                // distribute_actions с count == nframes вырождается
                // в «пометить все позиции» — ровно то, что нужно.
                distribute_actions(plan, nframes,
                                   static_cast<long>(nframes), Action::Dup);
            }
        } else {
            distribute_actions(plan, nframes, absd,
                               (delta > 0) ? Action::Dup : Action::Drop);
        }
    }

    FilePtr fout(std::fopen(args.outfile.c_str(), "wb"), &std::fclose);
    if (!fout) {
        println(stderr, "File {} could not be opened", args.outfile);
        return std::unexpected(Error::FileOpen);
    }

    // -------- 6. Конвейер reader/writer --------
    BoundedQueue<FrameBlock> q(kQueueCap);
    std::atomic<bool>        reader_ok{true};

    std::jthread reader([&](std::stop_token st) {
        const ReadStatus rs = for_each_frame(fin.get(), njunk, flen, codec,
            [&](std::span<const std::uint8_t> bytes, const FrameInfo& fi) {
                FrameBlock fb;
                fb.bytes.assign(bytes.begin(), bytes.end());
                fb.framesec = fi.framesec;
                return q.push(std::move(fb));
            }, st);

        if (rs == ReadStatus::Error)
            reader_ok.store(false, std::memory_order_relaxed);
        q.close();
    });

    double      written_sec = 0.0;
    std::size_t nfread      = 0;
    long        nf          = 0;
    bool        write_ok    = true;

    while (auto item = q.pop()) {
        if (nfread >= nframes) break;
        const Action a = plan[nfread];

        // Проверяем результат fwrite: переполнение диска и
        // ошибки ввода-вывода должны прерывать обработку, а не
        // молча давать усечённый выходной файл.
        auto put = [&](const std::vector<std::uint8_t>& bytes) -> bool {
            return std::fwrite(bytes.data(), 1, bytes.size(),
                               fout.get()) == bytes.size();
        };

        switch (a) {
            case Action::Copy:
                if (!put(item->bytes)) write_ok = false;
                written_sec += item->framesec;
                break;
            case Action::Dup:
                if (!put(item->bytes)) write_ok = false;
                if (!put(item->bytes)) write_ok = false;
                written_sec += 2.0 * item->framesec;
                ++nf;
                break;
            case Action::Drop:
                --nf;
                break;
        }
        ++nfread;
    }

    reader.request_stop();
    q.close();   // разблокировать reader, если он застрял в push()

    if (!reader_ok.load(std::memory_order_relaxed))
        return std::unexpected(Error::FileRead);

    // Сброс буфера и проверка отложенных ошибок записи:
    // переполнение диска часто проявляется только на flush.
    if (write_ok && std::fflush(fout.get()) != 0) write_ok = false;
    if (!write_ok)
        return std::unexpected(Error::FileWrite);

    if (nfread != nframes) {
        // Reader не смог выдать все кадры. Это не должно
        // происходить при корректном flen, но лучше сообщить
        // явно, чем молча выдать усечённый файл.
        print(stderr, "Warning: processed only {} of {} frames.\n",
              nfread, nframes);
    }

    print("{}: ", codec.long_name());
    if (delta > 0)      print("Inserted {} frames, ", nf);
    else if (delta < 0) print("Removed {} frames, ", -nf);
    else                print("No frames changed, ");
    print("actual length now is ");
    print_time(written_sec);
    print(" in {} frames.\n", static_cast<long>(nfread) + nf);

    return {};
}

} // namespace scale
