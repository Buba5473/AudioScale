/*
   dtscodec.cpp — реализация scale::Codec для DTS Coherent Acoustics.

   Refactored to C++23, 2024. Distributed under GNU GPL v2 or later.

   Класс DtsCodec инкапсулирует всё, что относится к DTS Core, а также
   автоматическое извлечение DTS Core из DTS-HD MA/HRA:

     - синхрослово DTS Core (16-bit BE = 0x7FFE8001, 16-bit LE = 0xFE7F0180);
     - разбор заголовка кадра (NBLKS, FSIZE, AMODE, SFREQ, BITRATE);
     - вычисление размера кадра и его длительности;
     - переопределённый frame_span(): сканирует следующий DTS Core sync,
       что автоматически отбрасывает XLL/X96-расширение DTS-HD и даёт на
       выходе чистый DTS Core;
     - печать человекочитаемой сводки о потоке.

   Таблицы соответствуют ETSI TS 102 114 (DTS Coherent Acoustics).

   Формат заголовка DTS Core (после 4-байтового sync, MSB-first):
     Frame Type                (1)
     Deficit Sample Count      (5)
     CRC Present               (1)
     Number of PCM Sample Blocks (7) [+1, 1..128]        → NBLKS
     Primary Frame Byte Size   (14) [+1, 1..16384]       → FSIZE
     Audio Channel Arrangement (6)                        → AMODE
     Core Audio Sampling Frequency (4)                    → SFREQ
     Transmission Bit Rate     (5)                        → BITRATE

   Кадры DTS Core самодостаточны, но в DTS-HD MA/HRA сразу за ядром идёт
   extension (XLL для MA, X96 для HRA) со своим sync-словом. Мы находим
   следующий DTS Core sync после ядра — всё, что между ними, является
   extension'ом и отбрасывается. Так ядро извлекается «бесплатно».
*/

#include "scale/common.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <string_view>

namespace {

// Единый набор using-деклараций, чтобы не квалифицировать имена
// в каждой сигнатуре. См. предыдущую ошибку сборки, где FrameInfo
// использовался внутри DtsCodec::frame_span без квалификации.
using scale::print;
using scale::Error;
using scale::FrameInfo;

// ---------------------------------------------------------------------------
// Таблицы DTS Core (ETSI TS 102 114)
// ---------------------------------------------------------------------------

// SFREQ (4 бита) -> частота дискретизации, Гц.
//   0x0 invalid, 0x1 8k, 0x2 16k, 0x3 32k, 0x4 64k, 0x5 128k,
//   0x6 11025, 0x7 22050, 0x8 44100, 0x9 88200, 0xA 176400,
//   0xB 12000, 0xC 24000, 0xD 48000, 0xE 96000, 0xF 192000.
constexpr std::array<double, 16> kFreqs = {
    0.0,      8000.0,   16000.0,  32000.0,
    64000.0,  128000.0, 11025.0,  22050.0,
    44100.0,  88200.0,  176400.0, 12000.0,
    24000.0,  48000.0,  96000.0,  192000.0
};

// BITRATE (5 бит) -> битрейт, кбит/с.
//   0x00..0x04 reserved, 0x05 = 128, ..., 0x18 = 1536,
//   0x19..0x1F reserved / user-defined.
constexpr std::array<std::size_t, 32> kBitrates = {
    0,    0,    0,    0,    0,      // 0x00..0x04 reserved
    128,  192,  224,  256,  320,    // 0x05..0x09
    384,  448,  512,  576,  640,    // 0x0A..0x0E
    768,  960,  1024, 1152, 1280,   // 0x0F..0x13
    1344, 1408, 1411, 1472, 1536,   // 0x14..0x18
    0,    0,    0,    0,    0,      // 0x19..0x1D reserved
    0,    0                          // 0x1E..0x1F user-defined
};

// AMODE[amode & 0x0F] -> число каналов (без LFE).
// Источник: FFmpeg libavcodec/dcadata.h, ff_dca_channels[16].
//   LFE (amode & 0x10) учитывается отдельно.
constexpr std::array<std::size_t, 16> kAmodeChannels = {
    1, 2, 2, 2, 2, 3, 3, 4, 4, 5, 6, 6, 6, 7, 8, 8
};

// Минимальный размер кадра DTS Core (FSIZE = 96, нижняя граница стандарта).
constexpr std::size_t kMinFrameBytes = 96;

// Максимальный размер кадра DTS Core: FSIZE = 14 бит, +1 → 16384.
constexpr std::size_t kMaxFrameBytes = 16384;

// ---------------------------------------------------------------------------
// Вспомогательные функции для побитового чтения MSB-first.
// ---------------------------------------------------------------------------

// 32-битное big-endian чтение из span (для sync-слова).
[[nodiscard]] constexpr std::uint32_t
read_be32(std::span<const std::uint8_t> b) noexcept {
    return (static_cast<std::uint32_t>(b[0]) << 24)
         | (static_cast<std::uint32_t>(b[1]) << 16)
         | (static_cast<std::uint32_t>(b[2]) << 8)
         |  static_cast<std::uint32_t>(b[3]);
}

// Извлечение N бит, начиная с битовой позиции bitpos (MSB-first),
// из массива байт. Используется для полей заголовка, не выровненных
// по байтовой границе. Вызывающий обязан гарантировать, что
// (bitpos + n + 7) / 8 <= b.size().
[[nodiscard]] constexpr std::uint32_t
read_bits(std::span<const std::uint8_t> b, std::size_t bitpos, unsigned n) noexcept {
    std::uint32_t v = 0;
    for (unsigned i = 0; i < n; ++i) {
        const std::size_t bp = bitpos + i;
        const std::size_t byte = bp >> 3;
        const unsigned   bit  = 7u - static_cast<unsigned>(bp & 7u);
        v = (v << 1) | ((b[byte] >> bit) & 1u);
    }
    return v;
}

// ---------------------------------------------------------------------------
// Реализация scale::Codec для DTS Core.
// ---------------------------------------------------------------------------

class DtsCodec final : public scale::Codec {
public:
    // -----------------------------------------------------------------------
    // Метаданные
    // -----------------------------------------------------------------------
    [[nodiscard]] std::string_view name()      const noexcept override { return "dts"; }
    [[nodiscard]] std::string_view long_name() const noexcept override { return "DTS"; }

    // -----------------------------------------------------------------------
    // Синхрослово DTS Core.
    //   16-bit BE: 0x7FFE8001
    //   16-bit LE: 0xFE7F0180 (байты sync-swap'нуты по 16-битным словам)
    // 14-bit BE (0x1FFFE800, DTS-in-WAV) намеренно не поддерживается для
    // парсинга — заголовок в нём хранится в 14-битных словах, что требует
    // отдельного битового ридера. Такие файлы лучше предварительно
    // конвертировать в 16-bit BE.
    // -----------------------------------------------------------------------
    [[nodiscard]] bool
    has_sync(std::span<const std::uint8_t> b) const noexcept override {
        if (b.size() < 4) return false;
        const std::uint32_t v = read_be32(b);
        return v == 0x7FFE8001u   // 16-bit BE
            || v == 0xFE7F0180u;  // 16-bit LE
    }

    // -----------------------------------------------------------------------
    // Разбор заголовка кадра DTS Core.
    // Требуется минимум 10 байт: 4 sync + 6 байт заголовка (43 бита).
    // -----------------------------------------------------------------------
    [[nodiscard]] std::expected<FrameInfo, Error>
    parse_header(std::span<const std::uint8_t> b) const noexcept override {
        if (b.size() < 10)
            return std::unexpected(Error::BadFormat);

        const std::uint32_t sync = read_be32(b);
        const bool is_16be = (sync == 0x7FFE8001u);
        const bool is_16le = (sync == 0xFE7F0180u);
        if (!is_16be && !is_16le)
            return std::unexpected(Error::BadFormat);

        // Заголовок начинается сразу после 4-байтового sync.
        // Для 16-bit LE байты 16-битных слов идут в обратном порядке —
        // разворачиваем их во временный буфер.
        std::array<std::uint8_t, 8> hdr_swap{};
        std::span<const std::uint8_t> hdr;
        if (is_16be) {
            hdr = b.subspan(4, 8);
        } else {
            for (std::size_t i = 0; i < 8; i += 2) {
                hdr_swap[i]     = b[4 + i + 1];
                hdr_swap[i + 1] = b[4 + i];
            }
            hdr = hdr_swap;
        }

        // Поля заголовка (MSB-first, от начала hdr):
        //   bit 0     Frame Type                (1)
        //   bit 1..5  Deficit Sample Count      (5)
        //   bit 6     CRC Present               (1)
        //   bit 7..13 Number of PCM Sample Blocks (7)  → NBLKS-1
        //   bit 14..27 Primary Frame Byte Size   (14)   → FSIZE-1
        //   bit 28..33 Audio Channel Arrangement (6)    → AMODE
        //   bit 34..37 Core Audio Sampling Frequency (4) → SFREQ
        //   bit 38..42 Transmission Bit Rate     (5)    → BITRATE
        const std::size_t nblks_raw = read_bits(hdr,  7,  7);
        const std::size_t fsize_raw = read_bits(hdr, 14, 14);
        const std::size_t amode     = read_bits(hdr, 28,  6);
        const std::size_t sfreq     = read_bits(hdr, 34,  4);
        const std::size_t brate     = read_bits(hdr, 38,  5);

        const std::size_t nblks = nblks_raw + 1;   // 1..128
        const std::size_t fsize = fsize_raw + 1;   // 1..16384

        if (sfreq >= kFreqs.size() || kFreqs[sfreq] == 0.0)
            return std::unexpected(Error::BadFormat);
        if (brate >= kBitrates.size() || kBitrates[brate] == 0)
            return std::unexpected(Error::BadFormat);
        if (fsize < kMinFrameBytes || fsize > kMaxFrameBytes)
            return std::unexpected(Error::BadFormat);
        if (amode > 63)
            return std::unexpected(Error::BadFormat);

        const std::size_t samples_per_frame = nblks * 32;

        FrameInfo fi{};
        fi.framebytes   = fsize;
        fi.framesec     = static_cast<double>(samples_per_frame) / kFreqs[sfreq];
        fi.freq_khz     = kFreqs[sfreq] / 1000.0;
        fi.bitrate_kbps = kBitrates[brate];
        return fi;
    }

    // -----------------------------------------------------------------------
    // frame_span() — ключевое отличие DTS от MP2/AC-3.
    //
    // Для MP2/AC-3 базовый Codec::frame_span() возвращает fi.framebytes,
    // и это корректно. Для DTS Core в DTS-HD MA/HRA сразу за ядром идёт
    // extension (XLL/X96) со своим sync'ом; следующий DTS Core sync
    // находится дальше. Найдя его, мы возвращаем полное расстояние от
    // текущего sync до следующего, а читающий код копирует только
    // fi.framebytes байт (само ядро) — extension отбрасывается.
    //
    //   * Fast path: если следующий sync стоит сразу за ядром (чистый
    //     DTS Core), возвращаем core.
    //   * Slow path: сканируем буфер начиная с core+1 до ближайшего sync.
    //   * Если sync не найден — возвращаем SIZE_MAX, движок должен
    //     дозаполнить буфер.
    //
    // Обратите внимание на квалификацию scale::FrameInfo — тип определён
    // в пространстве scale, а DtsCodec находится в анонимном namespace.
    // Без этой квалификации GCC 16+ выдаёт "'FrameInfo' does not name a type".
    // -----------------------------------------------------------------------
    [[nodiscard]] std::size_t
    frame_span(std::span<const std::uint8_t> buf,
               const scale::FrameInfo& fi) const noexcept override {
        constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();
        const std::size_t core = fi.framebytes;

        // Недостаточно данных, чтобы хотя бы проверить границу ядра.
        if (core + 4 > buf.size()) return kNone;

        // Fast path: чистый DTS Core.
        if (has_sync(buf.subspan(core))) return core;

        // Slow path: DTS-HD MA/HRA — ищем следующий DTS Core sync.
        // Начинаем с core+1, потому что позицию core мы уже проверили.
        for (std::size_t j = core + 1; j + 4 <= buf.size(); ++j) {
            if (has_sync(buf.subspan(j))) return j;
        }
        return kNone;
    }

    // -----------------------------------------------------------------------
    // Человекочитаемая сводка о потоке.
    // -----------------------------------------------------------------------
    void print_info(std::span<const std::uint8_t> b,
                    const scale::FrameInfo& fi) const override {
        if (b.size() < 10) return;

        const std::uint32_t sync = read_be32(b);
        const bool is_16be = (sync == 0x7FFE8001u);
        const bool is_16le = (sync == 0xFE7F0180u);
        if (!is_16be && !is_16le) return;

        // Разворачиваем 16-битные слова для LE-варианта.
        std::array<std::uint8_t, 8> hdr_swap{};
        std::span<const std::uint8_t> hdr;
        if (is_16be) {
            hdr = b.subspan(4, 8);
        } else {
            for (std::size_t i = 0; i < 8; i += 2) {
                hdr_swap[i]     = b[4 + i + 1];
                hdr_swap[i + 1] = b[4 + i];
            }
            hdr = hdr_swap;
        }

        const std::size_t nblks = read_bits(hdr,  7,  7) + 1;
        const std::size_t amode = read_bits(hdr, 28,  6);

        const std::size_t nch = kAmodeChannels[amode & 0x0F];
        const bool        lfe = (amode & 0x10) != 0;

        print("Audio type is DTS, {}, {} channel(s){}, {} samples per frame, "
              "sampling freq is {} kHz, and bitrate is {} kbps.\n",
              is_16le ? "16-bit LE" : "16-bit BE",
              nch, lfe ? ".1" : "",
              nblks * 32,
              fi.freq_khz, fi.bitrate_kbps);
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Фабрика
// ---------------------------------------------------------------------------

const scale::Codec& scale::dts_codec() {
    static const DtsCodec instance;
    return instance;
}
