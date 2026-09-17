/*
   mp2scale / ac3scale / dtsscale — общий движок.

   Copyright (c) 2004, by Zhuo Meng (zhuo@thunder.cwru.edu).
   Refactored to C++23, 2024. Distributed under GNU GPL v2 or later.

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
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
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
#  define SCALE_FSEEK(f, off, whence) std::fseek((f), (long)(off), (whence))
#endif

namespace scale {

namespace {

constexpr std::size_t kScanBuf  = 1u << 14;   // 16 KiB — окно поиска sync
constexpr std::size_t kOverlap  = 512;        // перекрытие окон head-scan
constexpr std::size_t kQueueCap = 64;         // глубина конвейера
constexpr std::size_t kMaxFrame = 65536;      // верхний предел размера кадра
constexpr std::size_t kMinFrame = 4;          // нижний предел размера кадра
constexpr std::size_t kBufSize  = 1u << 18;   // 256 KiB — хватает под DTS-HD
constexpr std::size_t kNone     = std::numeric_limits<std::size_t>::max();

using FilePtr = std::unique_ptr<std::FILE, int(*)(std::FILE*)>;

// ---------------------------------------------------------------------------
// Печать usage
// ---------------------------------------------------------------------------
void print_usage(std::string_view prg) {
    print(stderr,
        "Usage: {0} [-t target] [-o outfile] infile\n"
        "{0}: Scale MP2 / AC-3 / DTS Core audio to the target length\n"
        "          by duplicating or dropping whole frames. When infile\n"
        "          is DTS-HD MA/HRA, the DTS Core is extracted automatically.\n"
        "\n"
        "  -t target   Target length. Accepts:\n"
        "                 20.5        20.5 seconds (float)\n"
        "                 20.5s       20.5 seconds (explicit)\n"
        "                 500f        500 frames\n"
        "                 00:20:30    20 min 30 sec (hh:mm:ss)\n"
        "                 01:30       1 min 30 sec (mm:ss)\n"
        "              Without -t only reports stream properties.\n"
        "  -o outfile  Write scaled stream to outfile.\n"
        "  -h, --help  Show this message.\n", prg);
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

// ---------------------------------------------------------------------------
// Разбор double с полной проверкой.
// ---------------------------------------------------------------------------
bool parse_double(std::string_view s, double& out) {
    if (s.empty()) return false;
    try {
        std::size_t pos = 0;
        out = std::stod(std::string(s), &pos);
        if (pos != s.size()) return false;
        if (!std::isfinite(out) || out < 0.0) return false;
    } catch (...) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Bounds-safe поиск первого полного кадра в буфере.
// ---------------------------------------------------------------------------
std::optional<std::pair<std::size_t, FrameInfo>>
find_frame(std::span<const std::uint8_t> buf, const Codec& c) {
    const std::size_t n = buf.size();
    if (n < 10) return std::nullopt;

    for (std::size_t i = 0; i + 10 <= n; ++i) {
        auto view = buf.subspan(i);
        if (!c.has_sync(view)) continue;

        auto fi = c.parse_header(view);
        if (!fi) continue;

        const std::size_t core = fi->framebytes;
        if (core < kMinFrame || core > kMaxFrame) continue;
        if (i + core > n) continue;

        // Для MP2 / AC-3 сразу за ядром должен идти следующий sync —
        // это отсеивает случайные совпадения 0xFFF / 0x0B77 в мусоре.
        // Для DTS после ядра может быть XLL/HRA extension, поэтому
        // проверку не делаем (детектор всё равно попробует DTS первым).
        if (c.name() != "dts") {
            if (i + core + 4 > n) continue;
            if (!c.has_sync(buf.subspan(i + core))) continue;
        }

        return std::pair{i, *fi};
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Параллельный head-scan: режем окно на куски с перекрытием, каждый
// ищет первый валидный кадр, берём минимальный offset.
// ---------------------------------------------------------------------------
std::optional<std::pair<std::size_t, FrameInfo>>
parallel_head_scan(std::span<const std::uint8_t> head, const Codec& c) {
    if (head.empty()) return std::nullopt;

    const unsigned    hc    = std::max(1u, std::thread::hardware_concurrency());
    const std::size_t chunk = std::max<std::size_t>(1, (head.size() + hc - 1) / hc);

    std::vector<std::future<std::optional<std::pair<std::size_t, FrameInfo>>>> futs;
    futs.reserve(hc);

    for (unsigned t = 0; t < hc; ++t) {
        const std::size_t b = static_cast<std::size_t>(t) * chunk;
        if (b >= head.size()) break;
        const std::size_t e = std::min(head.size(), b + chunk + kOverlap);
        auto win = head.subspan(b, e - b);
        futs.push_back(std::async(std::launch::async,
            [win, &c, b]() -> std::optional<std::pair<std::size_t, FrameInfo>> {
                auto r = find_frame(win, c);
                if (!r) return std::nullopt;
                return std::pair{b + r->first, r->second};
            }));
    }

    std::optional<std::pair<std::size_t, FrameInfo>> best;
    for (auto& f : futs) {
        auto r = f.get();
        if (r && (!best || r->first < best->first))
            best = std::move(r);
    }
    return best;
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
            // Для DTS: extension уходит за окно — не можем отличить
            // валидные данные от мусора. Считаем окно валидным.
            end = tail.size();
        } else {
            end = i + span;
        }
        if (end > last_end) last_end = end;
    }

    if (last_end == 0) return 0;
    return tail.size() - last_end;
}

// ---------------------------------------------------------------------------
// Покадровый проход по потоку. Использует parse_header() + frame_span(),
// поэтому корректно обрабатывает MP2 padding и DTS-HD extension.
//
// Возвращает: число кадров (ядер) и суммарную длительность ядра.
// ---------------------------------------------------------------------------
struct ScanResult {
    std::size_t nframes   = 0;
    double      total_sec = 0.0;
    bool        ok        = false;
};

ScanResult scan_frames(std::FILE* fin, std::size_t start, std::size_t len,
                       const Codec& codec) {
    ScanResult r;
    if (SCALE_FSEEK(fin, static_cast<std::int64_t>(start), SEEK_SET) != 0)
        return r;

    std::vector<std::uint8_t> buf(kBufSize);
    std::size_t have = 0, pos = 0, remaining = len;

    while (remaining > 0) {
        if (pos == have) {
            have = std::fread(buf.data(), 1, buf.size(), fin);
            pos = 0;
            if (have == 0) break;
        }

        auto sp = std::span<const std::uint8_t>{buf.data() + pos, have - pos};
        auto fi = codec.parse_header(sp);
        if (!fi) break;

        const std::size_t core = fi->framebytes;
        std::size_t span = codec.frame_span(sp, *fi);

        if (span == kNone || pos + span > have) {
            if (have == buf.size()) {
                if (span == kNone) {
                    // Extension не найден в буфере — считаем кадр до конца.
                    span = std::min(remaining, have - pos);
                } else {
                    break;   // кадр не влезает даже в максимальный буфер
                }
            } else {
                std::memmove(buf.data(), buf.data() + pos, have - pos);
                have -= pos;
                pos = 0;
                std::size_t got = std::fread(buf.data() + have, 1,
                                             buf.size() - have, fin);
                if (got == 0) break;
                have += got;
                continue;
            }
        }

        if (core > span || span > remaining) break;

        pos       += span;
        remaining -= span;
        ++r.nframes;
        r.total_sec += fi->framesec;
    }

    r.ok = (remaining == 0);
    return r;
}

} // namespace

// ---------------------------------------------------------------------------
// Публичные функции
// ---------------------------------------------------------------------------

std::string_view to_string(Error e) noexcept {
    switch (e) {
        case Error::Help:        return "help requested";
        case Error::InvalidArgs: return "invalid arguments";
        case Error::FileOpen:    return "cannot open file";
        case Error::FileRead:    return "read error";
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
        std::vector<std::string_view> parts;
        std::size_t start = 0;
        while (start <= s.size()) {
            auto colon = s.find(':', start);
            auto end   = (colon == std::string_view::npos) ? s.size() : colon;
            parts.push_back(s.substr(start, end - start));
            if (colon == std::string_view::npos) break;
            start = colon + 1;
        }
        if (parts.size() < 2 || parts.size() > 3) return std::nullopt;

        static constexpr double kScale[3] = { 3600.0, 60.0, 1.0 };
        const std::size_t off = 3 - parts.size();

        double total = 0.0;
        for (std::size_t i = 0; i < parts.size(); ++i) {
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
        TargetSpec t;
        t.kind  = TargetSpec::Kind::Seconds;
        t.value = v;
        return t;
    }

    // --- суффикс 'f' / 'F' → кадры ---
    if (last == 'f' || last == 'F') {
        double v = 0.0;
        if (!parse_double(s.substr(0, s.size() - 1), v)) return std::nullopt;
        TargetSpec t;
        t.kind  = TargetSpec::Kind::Frames;
        t.value = v;
        return t;
    }

    // --- чистое число → секунды ---
    double v = 0.0;
    if (!parse_double(s, v)) return std::nullopt;
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

    if (argc < 2) {
        print_usage(prg);
        return std::unexpected(Error::Help);
    }

    bool have_infile = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view v = argv[i];

        if (v == "-h" || v == "--help" || v == "/?") {
            print_usage(prg);
            return std::unexpected(Error::Help);
        }

        // Определяем, опция это или путь. На Unix '/' — начало абсолютного
        // пути, а не префикс опции; учитываем это.
        bool is_opt = (!v.empty() && v[0] == '-');
#ifdef _WIN32
        if (!is_opt && v.size() > 1 && v[0] == '/' && v[1] != '/')
            is_opt = true;
#endif

        if (is_opt) {
            if (i + 1 >= argc) {
                print_usage(prg);
                return std::unexpected(Error::InvalidArgs);
            }
            const char opt = v.size() > 1 ? v[1] : '\0';
            switch (opt) {
            case 't': {
                auto spec = parse_target(argv[++i]);
                if (!spec) {
                    print(stderr, "Invalid -t value: {}\n", argv[i]);
                    return std::unexpected(Error::InvalidArgs);
                }
                a.target = *spec;
                break;
            }
            case 'o':
                a.outfile = argv[++i];
                break;
            default:
                print_usage(prg);
                return std::unexpected(Error::InvalidArgs);
            }
        } else {
            a.infile    = argv[i];
            have_infile = true;
        }
    }

    if (!have_infile) {
        print_usage(prg);
        return std::unexpected(Error::InvalidArgs);
    }
    return a;
}

// ---------------------------------------------------------------------------
// Печать времени HH:MM:SS.mmm
// ---------------------------------------------------------------------------
void print_time(double t) {
    if (!(t > 0.0)) t = 0.0;
    auto h = static_cast<unsigned long long>(t) / 3600ULL;
    t -= static_cast<double>(h) * 3600.0;
    auto m = static_cast<unsigned long long>(t) / 60ULL;
    t -= static_cast<double>(m) * 60.0;
    print("{:02}:{:02}:{:06.3f}", h, m, t);
}

// ---------------------------------------------------------------------------
// Детектор формата: DTS → AC-3 → MP2. DTS пробуется первым, потому что
// 0x7FFE8001 — самое длинное синхрослово и случайное совпадение маловероятно.
// ---------------------------------------------------------------------------
std::optional<Detected> detect_codec(std::span<const std::uint8_t> head) noexcept {
    const Codec* order[3] = { &dts_codec(), &ac3_codec(), &mp2_codec() };
    for (const Codec* c : order) {
        auto r = parallel_head_scan(head, *c);
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
        print(stderr, "File {} could not be opened\n", args.infile);
        return std::unexpected(Error::FileOpen);
    }

    // -------- размер файла --------
    std::error_code ec;
    const auto fsz_opt = std::filesystem::file_size(args.infile, ec);
    if (ec) return std::unexpected(Error::FileSeek);
    const std::size_t fsize = static_cast<std::size_t>(fsz_opt);

    if (fsize < 10) {
        print(stderr, "File {} is too short.\n", args.infile);
        return std::unexpected(Error::TooShort);
    }
    std::rewind(fin.get());

    // -------- 1. Детект формата --------
    const std::size_t head_len = std::min<std::size_t>(fsize, kScanBuf);
    std::vector<std::uint8_t> head(head_len);
    if (std::fread(head.data(), 1, head_len, fin.get()) != head_len
        && std::ferror(fin.get()))
        return std::unexpected(Error::FileRead);

    auto det = detect_codec(std::span<const std::uint8_t>{head});
    if (!det) {
        print(stderr,
              "File {} is not MP2, AC-3 or DTS (Core/HD).\n", args.infile);
        return std::unexpected(Error::BadFormat);
    }

    const Codec&    codec  = *det->codec;
    const std::size_t njunk = det->offset;
    const FrameInfo fi0     = det->fi;

    codec.print_info(std::span<const std::uint8_t>{head}.subspan(njunk), fi0);

    // -------- 2. Хвостовой мусор --------
    const std::size_t tail_len = std::min<std::size_t>(fsize, kScanBuf);
    std::vector<std::uint8_t> tail(tail_len);
    if (SCALE_FSEEK(fin.get(),
                    static_cast<std::int64_t>(fsize - tail_len),
                    SEEK_SET) != 0)
        return std::unexpected(Error::FileSeek);
    if (std::fread(tail.data(), 1, tail_len, fin.get()) != tail_len
        && std::ferror(fin.get()))
        return std::unexpected(Error::FileRead);

    const std::size_t ntrail =
        tail_junk_size(std::span<const std::uint8_t>{tail}, codec);

    if (fsize < njunk + ntrail) return std::unexpected(Error::TooShort);
    const std::size_t flen = fsize - njunk - ntrail;

    // -------- 3. Точный проход по потоку --------
    ScanResult scan = scan_frames(fin.get(), njunk, flen, codec);
    if (!scan.ok || scan.nframes == 0) {
        // Fallback: оценка по размеру первого кадра.
        scan.nframes   = flen / fi0.framebytes;
        scan.total_sec = fi0.framesec * static_cast<double>(flen)
                       / static_cast<double>(fi0.framebytes);
    }

    const std::size_t nframes = scan.nframes;
    const double      fsec    = scan.total_sec;
    const double      avg_sec = (nframes > 0)
        ? fsec / static_cast<double>(nframes)
        : fi0.framesec;

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

    // per — «сколько кадров приходится на одну вставку/удаление».
    double per = std::numeric_limits<double>::infinity();

    if (target_sec > 0.0 && nframes > 0) {
        const double delta = (target_sec - fsec) / avg_sec;  // в кадрах
        const int    n     = static_cast<int>(std::fabs(delta));

        print("Approximately {} frames need to be ", n);
        if (delta > 0.0)      print("inserted.\n");
        else if (delta < 0.0) print("dropped.\n");
        else                  print("nothing to do.\n");

        if (delta != 0.0)
            per = static_cast<double>(nframes) / delta;
    }

    if (args.outfile.empty()) return {};   // только отчёт

    // -------- 5. Открытие выхода и построение плана --------
    FilePtr fout(std::fopen(args.outfile.c_str(), "wb"), &std::fclose);
    if (!fout) {
        print(stderr, "File {} could not be opened\n", args.outfile);
        return std::unexpected(Error::FileOpen);
    }

    enum class Action : std::uint8_t { Copy, Drop, Dup };
    std::vector<Action> plan(nframes, Action::Copy);
    if (std::isfinite(per) && per > 0.0) {
        double mark = std::fabs(per);
        for (std::size_t i = 1; i <= nframes; ++i) {
            if (static_cast<double>(i) > mark) {
                plan[i - 1] = (per > 0.0) ? Action::Dup : Action::Drop;
                mark += std::fabs(per);
            }
        }
    }

    // -------- 6. Конвейер reader/writer --------
    BoundedQueue<FrameBlock> q(kQueueCap);
    std::atomic<bool>        reader_ok{true};

    std::jthread reader([&](std::stop_token st) {
        auto fail = [&] {
            reader_ok.store(false, std::memory_order_relaxed);
            q.close();
        };

        if (SCALE_FSEEK(fin.get(), static_cast<std::int64_t>(njunk),
                        SEEK_SET) != 0) { fail(); return; }

        std::vector<std::uint8_t> buf(kBufSize);
        std::size_t have = 0, pos = 0, remaining = flen;

        while (remaining > 0 && !st.stop_requested()) {
            if (pos == have) {
                have = std::fread(buf.data(), 1, buf.size(), fin.get());
                pos  = 0;
                if (have == 0) break;
            }

            auto sp = std::span<const std::uint8_t>{buf.data() + pos, have - pos};
            auto fi = codec.parse_header(sp);
            if (!fi) { fail(); return; }

            const std::size_t core = fi->framebytes;
            std::size_t span = codec.frame_span(sp, *fi);

            if (span == kNone || pos + span > have) {
                if (have == buf.size()) {
                    if (span == kNone) {
                        span = std::min(remaining, have - pos);
                    } else { fail(); return; }
                } else {
                    std::memmove(buf.data(), buf.data() + pos, have - pos);
                    have -= pos;
                    pos   = 0;
                    std::size_t got = std::fread(buf.data() + have, 1,
                                                 buf.size() - have, fin.get());
                    if (got == 0) break;
                    have += got;
                    continue;
                }
            }

            if (core > span || span > remaining) { fail(); return; }

            FrameBlock fb;
            fb.bytes.assign(buf.begin() + static_cast<std::ptrdiff_t>(pos),
                            buf.begin() + static_cast<std::ptrdiff_t>(pos + core));
            fb.framesec = fi->framesec;

            pos       += span;
            remaining -= span;

            if (!q.push(std::move(fb))) break;
        }
        q.close();
    });

    double      written_sec = 0.0;
    std::size_t nfread      = 0;
    long        nf          = 0;

    while (auto item = q.pop()) {
        if (nfread >= nframes) break;
        const Action a = plan[nfread];
        const auto   w = item->bytes.size();

        switch (a) {
            case Action::Copy:
                std::fwrite(item->bytes.data(), 1, w, fout.get());
                written_sec += item->framesec;
                break;
            case Action::Dup:
                std::fwrite(item->bytes.data(), 1, w, fout.get());
                std::fwrite(item->bytes.data(), 1, w, fout.get());
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

    if (per > 0.0) print("Inserted {} frames, ", nf);
    else           print("Removed {} frames, ", -nf);
    print("actual length now is ");
    print_time(written_sec);
    print(" in {} frames.\n", static_cast<long>(nfread) + nf);

    return {};
}

} // namespace scale
