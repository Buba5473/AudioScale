/*
   mp2codec.cpp — реализация scale::Codec для MPEG-1 Layer II.

   Copyright (c) 2004, by Zhuo Meng (zhuo@thunder.cwru.edu).
   Refactored to C++23, 2024. Distributed under GNU GPL v2 or later.

   Класс Mp2Codec инкапсулирует всё, что относится к формату MP2:
     - синхрослово 0xFFF и его проверку;
     - разбор заголовка кадра (layer, битрейт, частота, padding, CRC);
     - вычисление размера кадра и его длительности;
     - печать человекочитаемой сводки о потоке.

   Важно:
     * никаких статических глобальных буферов — всё работает через
       std::span, границы проверяются на стороне вызывающего (common.cpp);
     * размер кадра может меняться на 1 байт из-за бита padding, поэтому
       движок обязан вызывать parse_header() на каждом кадре — базовый
       Codec::frame_span() возвращает fi.framebytes, что здесь корректно,
       так как parse_header уже учёл padding в framebytes.
*/

#include "scale/common.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace {

// Единый набор using-деклараций, чтобы не квалифицировать имена в каждой
// сигнатуре (см. предыдущую ошибку сборки в dtscodec.cpp).
using scale::print;
using scale::Error;
using scale::FrameInfo;

// ---------------------------------------------------------------------------
// Таблицы MPEG-1 Layer II (ISO/IEC 11172-3)
// ---------------------------------------------------------------------------

// Индекс битрейта (4 бита) -> кбит/с.
//   0000 = free, 1111 = reserved/bad. Оба случая отклоняются.
constexpr std::array<std::size_t, 16> kBitrates = {
    0,     // 0000 — free
    32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384,
    0      // 1111 — reserved / bad
};

// Индекс частоты (2 бита) -> кГц. 3 => reserved.
// Порядок для MPEG-1: 44.1, 48, 32. Оригинальные утилиты поддерживали
// только MPEG-1 — сохраняем это поведение.
constexpr std::array<double, 4> kFreqs = { 44.1, 48.0, 32.0, 0.0 };

// Число сэмплов на кадр для Layer II (MPEG-1).
constexpr std::size_t kSamplesPerFrame = 1152;

// Минимально допустимый размер кадра — отсекаем мусор.
constexpr std::size_t kMinFrameBytes = 4;

// ---------------------------------------------------------------------------
// Реализация scale::Codec для MP2
// ---------------------------------------------------------------------------

class Mp2Codec final : public scale::Codec {
public:
    // -----------------------------------------------------------------------
    // Метаданные
    // -----------------------------------------------------------------------
    [[nodiscard]] std::string_view name()      const noexcept override { return "mp2"; }
    [[nodiscard]] std::string_view long_name() const noexcept override { return "MP2"; }

    // -----------------------------------------------------------------------
    // Синхрослово: 11 бит "1111 1111 111".
    // На практике достаточно b[0] == 0xFF и старших 4 бит второго байта.
    // -----------------------------------------------------------------------
    [[nodiscard]] bool
    has_sync(std::span<const std::uint8_t> b) const noexcept override {
        return b.size() >= 2
            && b[0] == 0xFF
            && (b[1] & 0xF0) == 0xF0;
    }

    // -----------------------------------------------------------------------
    // Разбор заголовка кадра.
    // Требуется минимум 4 байта (заголовок MPEG-1 Layer II без CRC).
    //
    // Раскладка байтов (MSB-first):
    //   b[0]: 11111111                — sync (11 бит)
    //   b[1]: 111VVLLP                — VV=версия, LL=layer, P=protection
    //   b[2]: BBBBFFFP                — BBBB=битрейт, FFF=частота, P=padding
    //   b[3]: MMCC....                — MM=channel mode, CC=mode extension
    // -----------------------------------------------------------------------
    [[nodiscard]] std::expected<FrameInfo, Error>
    parse_header(std::span<const std::uint8_t> b) const noexcept override {
        if (b.size() < 4)
            return std::unexpected(Error::BadFormat);

        const std::uint8_t b1 = b[1];
        const std::uint8_t b2 = b[2];

        // Layer (биты 2..1): 01=Layer III, 10=Layer II, 11=Layer I.
        // Нас интересует только Layer II.
        const int layer = 4 - ((b1 & 0x06) >> 1);
        if (layer != 2)
            return std::unexpected(Error::BadFormat);

        // Индексы битрейта / частоты / padding.
        const std::size_t brInd   = (b2 & 0xF0) >> 4;
        const std::size_t freqInd = (b2 & 0x0C) >> 2;
        const bool        pad     = (b2 & 0x02) != 0;

        // Валидация индексов — иначе OOB по массивам и деление на ноль.
        if (brInd >= kBitrates.size() || kBitrates[brInd] == 0)
            return std::unexpected(Error::BadFormat);
        if (freqInd >= 3 || kFreqs[freqInd] == 0.0)
            return std::unexpected(Error::BadFormat);

        // Формула размера кадра MPEG-1 Layer II
        // (битрейт в кбит/с, частота в кГц):
        //     frame_size = bitrate * 144 / sample_rate + padding
        // где 144 = 1152 сэмпла / 8 бит * 1000 (единицы кбит/с, кГц).
        //
        // Вычисление в double, чтобы избежать -Wconversion при приведении
        // size_t → double на этапе деления на kFreqs[freqInd].
        const double frame_size_f =
            static_cast<double>(kBitrates[brInd]) * 144.0 / kFreqs[freqInd];
        const std::size_t framebytes =
            static_cast<std::size_t>(frame_size_f) + (pad ? 1u : 0u);

        if (framebytes < kMinFrameBytes)
            return std::unexpected(Error::BadFormat);

        FrameInfo fi{};
        fi.framebytes   = framebytes;
        fi.framesec     = static_cast<double>(kSamplesPerFrame)
                        / (kFreqs[freqInd] * 1000.0);
        fi.freq_khz     = kFreqs[freqInd];
        fi.bitrate_kbps = kBitrates[brInd];
        return fi;
    }

    // -----------------------------------------------------------------------
    // frame_span() для MP2 не переопределяем.
    //
    // После корректного парсинга framebytes уже включает padding, поэтому
    // следующий sync находится ровно на fi.framebytes от текущего. Базовая
    // реализация Codec::frame_span() возвращает именно fi.framebytes —
    // этого достаточно.
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    // Человекочитаемая сводка о потоке.
    // -----------------------------------------------------------------------
    void print_info(std::span<const std::uint8_t> b,
                    const scale::FrameInfo& fi) const override {
        if (b.size() < 4) return;

        const std::uint8_t b1 = b[1];
        const std::uint8_t b3 = b[3];

        // MPEG audio version: 00=2.5, 01=reserved, 10=MPEG-2, 11=MPEG-1.
        // Оригинал работал только с MPEG-1, но формат сводки сохраняем.
        const int mpgv     = 2 - ((b1 & 0x08) >> 3);   // 2 => MPEG-2, 1 => MPEG-1
        const int protect  = 1 - (b1 & 0x01);          // 1 => есть CRC

        // Channel mode (биты 7..6 третьего байта):
        //   00 stereo, 01 joint stereo, 10 dual channel, 11 mono.
        // Исправлено: было `(b3 && 0xC0) >> 6` — логическое И вместо
        // побитового; из-за этого режим всегда выводился как "stereo".
        const int chanmode = (b3 & 0xC0) >> 6;

        print("Audio type is MPEG-{}, Layer 2, ", mpgv);
        switch (chanmode) {
            case 0: print("stereo, ");       break;
            case 1: print("joint stereo, "); break;
            case 2: print("dual channel, "); break;
            case 3: print("mono, ");         break;
            default: break;
        }
        print("{}, each frame is {} bytes, sampling freq is {} kHz, "
              "and bitrate is {} kbps.\n",
              protect ? "with CRC" : "without CRC",
              fi.framebytes, fi.freq_khz, fi.bitrate_kbps);
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Фабрика
// ---------------------------------------------------------------------------

const scale::Codec& scale::mp2_codec() {
    static const Mp2Codec instance;
    return instance;
}
