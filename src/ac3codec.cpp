/*
   ac3codec.cpp — реализация scale::Codec для AC-3 (Dolby Digital).

   Copyright (c) 2004, by Zhuo Meng (zhuo@thunder.cwru.edu).
   Refactored to C++23, 2024. Distributed under GNU GPL v2 or later.

   Класс Ac3Codec инкапсулирует всё, что относится к формату AC-3:
     - синхрослово 0x0B77 и его проверку;
     - разбор заголовка кадра (fscod / frmsizecod / acmod / bsmod / lfeon);
     - вычисление размера кадра в байтах и его длительности;
     - печать человекочитаемой сводки о потоке (версия, тип сервиса,
       конфигурация каналов, LFE).

   Таблицы соответствуют ATSC A/52 (AC-3), §5.3 "Sync Frame".
   Размеры кадра заданы отдельно для 48, 44.1 и 32 кГц — это устраняет
   ошибку предыдущей версии, где для 44.1 и 32 кГц использовались
   некорректные формулы, дававшие абсурдно малые значения.
*/

#include "scale/common.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace {

using scale::print;

// ---------------------------------------------------------------------------
// Таблицы AC-3 (ATSC A/52)
// ---------------------------------------------------------------------------

// frmsizecod (6 бит) -> битрейт, кбит/с. Индексы 0..37 валидны,
// 38..63 зарезервированы. Значения идут парами: для каждой пары
// (2k, 2k+1) битрейт одинаков, отличается только выбор размера кадра.
constexpr std::array<std::size_t, 38> kBitrates = {
    32,  32,  40,  40,  48,  48,  56,  56,
    64,  64,  80,  80,  96,  96, 112, 112,
   128, 128, 160, 160, 192, 192, 224, 224,
   256, 256, 320, 320, 384, 384, 448, 448,
   512, 512, 576, 576, 640, 640
};

// fscod (2 бита) -> частота дискретизации, кГц.
//   00 = 48 kHz, 01 = 44.1 kHz, 10 = 32 kHz, 11 = reserved.
constexpr std::array<double, 4> kFreqs = { 48.0, 44.1, 32.0, 0.0 };

// Число сэмплов на кадр AC-3 (фиксировано стандартом).
constexpr std::size_t kSamplesPerFrame = 1536;

// Минимально допустимый размер кадра в байтах:
//   2 sync + 2 crc1 + 2 bSI + 2 bsi — минимальный валидный кадр.
constexpr std::size_t kMinFrameBytes = 8;

// Максимальный размер кадра AC-3 = 3840 байт (640 кбит/с при 32 кГц,
// 1536 сэмплов). Верхняя защита от мусорных frmsizecod.
constexpr std::size_t kMaxFrameBytes = 3840;

// Размер кадра в 16-битных словах, индексируется [frmsizecod][fscod].
// Колонки: 48 кГц, 44.1 кГц, 32 кГц.
//
// Источник: ATSC A/52 §5.3 (совпадает с ff_ac3_frame_size_tab из FFmpeg).
// Раньше для 44.1 и 32 кГц использовались некорректные формулы,
// дававшие абсурдно малые значения (например, 1 слово вместо ~70),
// из-за чего AC-3 на этих частотах вообще не масштабировался.
constexpr std::array<std::array<std::size_t, 3>, 38> kFrameSizeWords = {{
    {  64,  69,  96 }, {  64,  70,  96 },
    {  80,  87, 120 }, {  80,  88, 120 },
    {  96, 104, 144 }, {  96, 105, 144 },
    { 112, 121, 168 }, { 112, 122, 168 },
    { 128, 139, 192 }, { 128, 140, 192 },
    { 160, 174, 240 }, { 160, 175, 240 },
    { 192, 208, 288 }, { 192, 209, 288 },
    { 224, 243, 336 }, { 224, 244, 336 },
    { 256, 278, 384 }, { 256, 279, 384 },
    { 320, 348, 480 }, { 320, 349, 480 },
    { 384, 417, 576 }, { 384, 418, 576 },
    { 448, 487, 672 }, { 448, 488, 672 },
    { 512, 557, 768 }, { 512, 558, 768 },
    { 640, 696, 960 }, { 640, 697, 960 },
    { 768, 835,1152 }, { 768, 836,1152 },
    { 896, 975,1344 }, { 896, 976,1344 },
    {1024,1114,1536 }, {1024,1115,1536 },
    {1152,1253,1728 }, {1152,1254,1728 },
    {1280,1393,1920 }, {1280,1394,1920 }
}};

// ---------------------------------------------------------------------------
// Реализация scale::Codec для AC-3
// ---------------------------------------------------------------------------

class Ac3Codec final : public scale::Codec {
public:
    // -----------------------------------------------------------------------
    // Метаданные
    // -----------------------------------------------------------------------
    [[nodiscard]] std::string_view name()      const noexcept override { return "ac3"; }
    [[nodiscard]] std::string_view long_name() const noexcept override { return "AC-3"; }

    // -----------------------------------------------------------------------
    // Синхрослово AC-3: 0x0B77 (16 бит, big-endian).
    // -----------------------------------------------------------------------
    [[nodiscard]] bool
    has_sync(std::span<const std::uint8_t> b) const noexcept override {
        return b.size() >= 2
            && b[0] == 0x0B
            && b[1] == 0x77;
    }

    // -----------------------------------------------------------------------
    // Разбор заголовка кадра.
    //
    // Требуется минимум 8 байт:
    //   b[0..1]  sync 0x0B77
    //   b[2..3]  crc1
    //   b[4]     fscod (2) | frmsizecod (6)
    //   b[5]     bsid  (5) | bsmod   (3)
    //   b[6]     acmod (3) | cmixlev/surmixlev/dsurmod/lfeon (остальное)
    //   b[7]     ...продолжение bsi...
    // -----------------------------------------------------------------------
    [[nodiscard]] std::expected<scale::FrameInfo, scale::Error>
    parse_header(std::span<const std::uint8_t> b) const noexcept override {
        using scale::Error;
        using scale::FrameInfo;

        if (b.size() < 8)
            return std::unexpected(Error::BadFormat);

        // Байт 4: fscod в старших 2 битах, frmsizecod в младших 6.
        const std::uint8_t b4 = b[4];

        const std::size_t fscod      = (b4 & 0xC0) >> 6;
        const std::size_t frmsizecod = b4 & 0x3F;

        // Валидация индексов — иначе OOB и деление на ноль.
        if (fscod >= 3 || kFreqs[fscod] == 0.0)
            return std::unexpected(Error::BadFormat);
        if (frmsizecod >= kBitrates.size())
            return std::unexpected(Error::BadFormat);

        // Размер кадра берём напрямую из таблицы ATSC A/52.
        const std::size_t words      = kFrameSizeWords[frmsizecod][fscod];
        const std::size_t framebytes = words * 2;

        if (framebytes < kMinFrameBytes || framebytes > kMaxFrameBytes)
            return std::unexpected(Error::BadFormat);

        FrameInfo fi{};
        fi.framebytes   = framebytes;
        fi.framesec     = static_cast<double>(kSamplesPerFrame)
                        / (kFreqs[fscod] * 1000.0);
        fi.freq_khz     = kFreqs[fscod];
        fi.bitrate_kbps = kBitrates[frmsizecod];
        return fi;
    }

    // -----------------------------------------------------------------------
    // frame_span() для AC-3 не переопределяем.
    //
    // Кадры AC-3 фиксированного размера; frmsizecod кодирует уже финальный
    // размер, отдельного padding-бита нет. Следующий sync находится ровно
    // на fi.framebytes от текущего — базовая реализация Codec::frame_span()
    // возвращает именно это значение.
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    // Человекочитаемая сводка о потоке.
    // -----------------------------------------------------------------------
    void print_info(std::span<const std::uint8_t> b,
                    const scale::FrameInfo& fi) const override {
        if (b.size() < 8) return;

        // Байт 5: bsid (5 бит) + bsmod (3 бита).
        // Байт 6: acmod (3 бита) + остальное.
        const std::uint8_t b5 = b[5];
        const std::uint8_t b6 = b[6];

        const std::size_t ac3v  = (b5 & 0xF8) >> 3;   // bsid, обычно 8 (AC-3) или 6 (ранний)
        const std::size_t bsmod = b5 & 0x07;
        const std::size_t acmod = (b6 & 0xE0) >> 5;

        // Режимы каналов AC-3 (acmod, 3 бита):
        //   0 = 1+1 (Ch1,Ch2)
        //   1 = 1/0 (C)
        //   2 = 2/0 (L,R)
        //   3 = 3/0 (L,C,R)
        //   4 = 2/1 (L,R,S)
        //   5 = 3/1 (L,C,R,S)
        //   6 = 2/2 (L,R,SL,SR)
        //   7 = 3/2 (L,C,R,SL,SR)
        static constexpr std::array<std::string_view, 8> kChanMode = {
            "1+1", "1/0", "2/0", "3/0", "2/1", "3/1", "2/2", "3/2"
        };
        static constexpr std::array<std::size_t, 8> kNfChans = {
            2, 1, 2, 3, 3, 4, 4, 5
        };

        // Позиция lfeon зависит от acmod:
        //   acmod 1/0  — сдвиг 4;
        //   acmod 1+1  — сдвиг 4;
        //   acmod 2/0  — сдвиг 4;
        //   acmod 3/0  — сдвиг 3 (2 бита cmixlev);
        //   acmod 2/1  — сдвиг 4;
        //   acmod 3/1  — сдвиг 3;
        //   acmod 2/2  — сдвиг 3 (2 бита dsurmod);
        //   acmod 3/2  — сдвиг 2 (cmixlev + surmixlev).
        // Упрощённо: bitshift начинается с 4 и уменьшается на 2 за каждый
        // присутствующий многоуровневый параметр.
        std::size_t shift = 4;
        if ((acmod & 0x01) && acmod != 0x01) shift -= 2;   // cmixlev
        if ((acmod & 0x04) || acmod == 0x02) shift -= 2;   // surmixlev / dsurmod
        const bool lfe = ((b6 >> shift) & 1u) != 0;

        // bsmod: 0 = complete main, 1 = music/effects, 2 = visually impaired,
        //        3 = hearing impaired, 4 = dialogue, 5 = commentary,
        //        6 = emergency, 7 = voice-over (для acmod > 1).
        // "Main audio service" — bsmod 0 или 1, а также 7 для многоканальных.
        print("Audio type is AC-3, version {}, ", ac3v);
        if (bsmod < 2 || (bsmod == 7 && acmod > 1))
            print("main audio service, ");
        else
            print("associated service, ");

        print("{}{} channel(s) mode {}.\n",
              kNfChans[acmod], lfe ? ".1" : "", kChanMode[acmod]);

        print("Each frame is {} bytes, sampling freq is {} kHz, "
              "and bitrate is {} kbps.\n",
              fi.framebytes, fi.freq_khz, fi.bitrate_kbps);
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Фабрика
// ---------------------------------------------------------------------------

const scale::Codec& scale::ac3_codec() {
    static const Ac3Codec instance;
    return instance;
}
