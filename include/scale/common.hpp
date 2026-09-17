#pragma once
/*
   mp2scale / ac3scale / dtsscale — общий заголовок.

   Copyright (c) 2004, by Zhuo Meng (zhuo@thunder.cwru.edu).
   Refactored to C++23, 2024. Distributed under GNU GPL v2 or later.

   Здесь собраны:
     - типы ошибок и их строковые представления;
     - структуры TargetSpec / FrameInfo / Args;
     - абстрактный интерфейс Codec (MP2 / AC-3 / DTS Core);
     - фабрики конкретных кодеков;
     - детектор формата по головному буферу;
     - публичные точки входа: parse_args(), parse_target(),
       print_time(), scale_stream().

   Заголовок не содержит тяжёлых зависимостей (только <cstddef>,
   <cstdint>, <expected>, <optional>, <span>, <string>, <string_view>
   плюс print.hpp) — это позволяет использовать его и в main-обёртке,
   и в unit-тестах без лишних include.
*/

#include "scale/print.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace scale {

// ---------------------------------------------------------------------------
// Ошибки
// ---------------------------------------------------------------------------

enum class Error : std::uint8_t {
    Help,          // запрошена справка (-h / --help) — не является ошибкой
    InvalidArgs,   // некорректные аргументы командной строки
    FileOpen,      // не удалось открыть входной/выходной файл
    FileRead,      // ошибка чтения
    FileSeek,      // ошибка позиционирования
    BadFormat,     // поток не распознан как MP2 / AC-3 / DTS
    TooShort,      // файл слишком мал для анализа
};

[[nodiscard]] std::string_view to_string(Error e) noexcept;

// ---------------------------------------------------------------------------
// Целевая длительность (-t). Поддерживает три формы:
//   - секунды:       "20.5"   "20.5s"
//   - кадры:         "500f"
//   - время:         "mm:ss"  "hh:mm:ss"  (можно с дробной частью секунд)
// ---------------------------------------------------------------------------

struct TargetSpec {
    enum class Kind : std::uint8_t {
        Seconds,   // value — секунды
        Frames,    // value — число кадров
    };

    Kind   kind  = Kind::Seconds;
    double value = 0.0;

    [[nodiscard]] bool active() const noexcept { return value > 0.0; }
};

// ---------------------------------------------------------------------------
// Информация о кадре
// ---------------------------------------------------------------------------

struct FrameInfo {
    std::size_t framebytes   = 0;    // размер ядра кадра (без DTS-HD extension)
    double      framesec     = 0.0;  // длительность одного кадра, сек
    double      freq_khz     = 0.0;  // частота дискретизации, кГц
    std::size_t bitrate_kbps = 0;    // битрейт, кбит/с
};

// ---------------------------------------------------------------------------
// Аргументы командной строки
// ---------------------------------------------------------------------------

struct Args {
    TargetSpec  target;    // не active() => только отчёт, без масштабирования
    std::string outfile;   // пусто => без записи выходного файла
    std::string infile;
};

// ---------------------------------------------------------------------------
// Интерфейс кодека
//
//   Все методы — noexcept и bounds-safe: любая проверка размера буфера
//   выполняется внутри parse_header / frame_span, ошибки возвращаются
//   через std::expected. Движок (common.cpp) никогда не читает буфер
//   напрямую, а только через эти методы.
// ---------------------------------------------------------------------------

class Codec {
public:
    virtual ~Codec() = default;

    Codec(const Codec&)            = delete;
    Codec& operator=(const Codec&) = delete;

    // Человекочитаемые имена: "mp2" / "ac3" / "dts" и "MP2" / "AC-3" / "DTS".
    [[nodiscard]] virtual std::string_view name()      const noexcept = 0;
    [[nodiscard]] virtual std::string_view long_name() const noexcept = 0;

    // Проверить синхрослово в начале span (нужно 2..4 байта для
    // MP2/AC-3 и 4 байта для DTS Core / DTS-HD).
    [[nodiscard]] virtual bool
        has_sync(std::span<const std::uint8_t> buf) const noexcept = 0;

    // Разобрать заголовок кадра. Требует минимум 4..10 байт (зависит
    // от формата). Возвращает FrameInfo либо BadFormat / TooShort.
    [[nodiscard]] virtual std::expected<FrameInfo, Error>
        parse_header(std::span<const std::uint8_t> buf) const noexcept = 0;

    // Расстояние от текущего sync до следующего (>= fi.framebytes).
    //   * SIZE_MAX → данных недостаточно, движок должен дозаполнить буфер;
    //   * базовое поведение — вернуть fi.framebytes (MP2, AC-3);
    //   * DTS Core переопределяет: сканирует следующий sync, что
    //     автоматически отбрасывает XLL/HRA-расширение DTS-HD и даёт
    //     на выходе чистый DTS Core.
    [[nodiscard]] virtual std::size_t
        frame_span(std::span<const std::uint8_t> buf,
                   const FrameInfo& fi) const noexcept {
        (void)buf;
        return fi.framebytes;
    }

    // Напечатать человекочитаемую сводку о потоке (использует первый
    // кадр и уже распарсенный FrameInfo).
    virtual void print_info(std::span<const std::uint8_t> first_frame,
                            const FrameInfo& fi) const = 0;

protected:
    Codec() = default;
};

// Фабрики конкретных кодеков (thread-safe: статические локальные).
[[nodiscard]] const Codec& mp2_codec();
[[nodiscard]] const Codec& ac3_codec();
[[nodiscard]] const Codec& dts_codec();

// ---------------------------------------------------------------------------
// Детектор формата
// ---------------------------------------------------------------------------

struct Detected {
    const Codec* codec  = nullptr;
    std::size_t  offset = 0;   // смещение первого валидного кадра
    FrameInfo    fi{};
};

// Пробует DTS → AC-3 → MP2. Для DTS-HD MA входной поток содержит
// DTS Core в начале кадра, поэтому детектируется тем же синхрословом.
[[nodiscard]] std::optional<Detected>
    detect_codec(std::span<const std::uint8_t> head) noexcept;

// ---------------------------------------------------------------------------
// Публичные точки входа
// ---------------------------------------------------------------------------

// Разобрать argc/argv. Возвращает Args либо:
//   * Error::Help        — при -h / --help / отсутствии аргументов (usage уже напечатан);
//   * Error::InvalidArgs — при некорректных опциях (usage уже напечатан).
// Обработка ошибки остаётся за вызывающим (main).
[[nodiscard]] std::expected<Args, Error>
    parse_args(int argc, char** argv, std::string_view prg);

// Разобрать значение -t. Возвращает nullopt при невалидной строке.
//   "20.5"      → 20.5 s
//   "20.5s"     → 20.5 s
//   "500f"      → 500 frames
//   "00:20:30"  → 20 min 30 sec
//   "01:30"     → 1 min 30 sec
[[nodiscard]] std::optional<TargetSpec>
    parse_target(std::string_view s) noexcept;

// Печать времени в формате HH:MM:SS.mmm.
void print_time(double t);

// Полный цикл: детект формата, анализ файла, построение плана и (если
// задан outfile) масштабирование потока. Многопоточно: параллельный
// head-scan + конвейер reader/writer. Внутри сам выбирает Codec через
// detect_codec(), поэтому принимает только Args.
[[nodiscard]] std::expected<void, Error>
    scale_stream(const Args& args);

} // namespace scale
