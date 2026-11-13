#pragma once
/*
   tests/test_common.hpp — минимальный самодельный тест-фреймворк.

   Не тянет внешних зависимостей: регистрация тестов через статические
   объекты, запуск — из tests/test_main.cpp.
*/

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace scale::test {

using TestFn = void (*)();

struct TestCase {
    const char* name;
    TestFn      fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

inline int& failure_count() {
    static int n = 0;
    return n;
}

struct Registrar {
    Registrar(const char* name, TestFn fn) { registry().push_back({name, fn}); }
};

} // namespace scale::test

#define SCALE_TEST(name)                                              \
    static void name();                                               \
    static ::scale::test::Registrar reg_##name(#name, &name);         \
    static void name()

#define SCALE_CHECK(cond)                                             \
    do {                                                              \
        if (!(cond)) {                                                \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n",       \
                         __FILE__, __LINE__, #cond);                  \
            throw std::runtime_error("check failed: " #cond);         \
        }                                                             \
    } while (0)

#define SCALE_CHECK_EQ(a, b)                                          \
    do {                                                              \
        auto _va = (a);                                               \
        auto _vb = (b);                                               \
        if (!(_va == _vb)) {                                          \
            std::fprintf(stderr, "CHECK_EQ failed at %s:%d: %s == %s\n",\
                         __FILE__, __LINE__, #a, #b);                 \
            throw std::runtime_error("check_eq failed");              \
        }                                                             \
    } while (0)


// ---------------------------------------------------------------------------
// Общие хелперы для синтетических кадров.
// Используются как в test_detect.cpp, так и в test_roundtrip.cpp,
// чтобы не дублировать битовую раскладку заголовков.
// ---------------------------------------------------------------------------

// MPEG-1, Layer II, 192 кбит/с, 48 кГц, без CRC, stereo.
// frame_size = 192 * 144 / 48 = 576 байт.
inline std::vector<std::uint8_t> make_mp2_frame() {
    std::vector<std::uint8_t> f(576, 0x00);
    f[0] = 0xFF;
    f[1] = 0xFD;
    f[2] = 0xA4;
    f[3] = 0x00;
    return f;
}

// N последовательных MP2-кадров.
inline std::vector<std::uint8_t>
make_mp2_stream(std::size_t nframes) {
    // Строится из make_mp2_frame() — единственный источник
    // истины для байтовой раскладки заголовка MP2.
    const auto frame = make_mp2_frame();
    std::vector<std::uint8_t> data;
    data.reserve(nframes * frame.size());
    for (std::size_t i = 0; i < nframes; ++i)
        data.insert(data.end(), frame.begin(), frame.end());
    return data;
}

// AC-3: 48 kHz, frmsizecod=20 → 384 слова → 768 байт (192 кбит/с).
inline std::vector<std::uint8_t> make_ac3_frame() {
    std::vector<std::uint8_t> f(768, 0x00);
    f[0] = 0x0B; f[1] = 0x77;   // sync
    f[4] = 0x14;                // fscod=0 (48 kHz), frmsizecod=20
    f[5] = 0x40;                // bsid=8
    return f;
}

// N последовательных AC-3-кадров.
inline std::vector<std::uint8_t>
make_ac3_stream(std::size_t nframes) {
    const auto frame = make_ac3_frame();
    std::vector<std::uint8_t> data;
    data.reserve(nframes * frame.size());
    for (std::size_t i = 0; i < nframes; ++i)
        data.insert(data.end(), frame.begin(), frame.end());
    return data;
}

// DTS Core: 48 kHz, 1536 кбит/с, NBLKS=16, FSIZE=96, AMODE=2.
inline std::vector<std::uint8_t> make_dts_frame() {
    std::vector<std::uint8_t> f(96, 0x00);
    f[0] = 0x7F; f[1] = 0xFE; f[2] = 0x80; f[3] = 0x01;
    f[4] = 0x02; f[5] = 0x3C; f[6] = 0x05;
    f[7] = 0xF0; f[8] = 0xB7; f[9] = 0x00;
    return f;
}

// N последовательных DTS Core кадров.
inline std::vector<std::uint8_t>
make_dts_stream(std::size_t nframes) {
    const auto frame = make_dts_frame();
    std::vector<std::uint8_t> data;
    data.reserve(nframes * frame.size());
    for (std::size_t i = 0; i < nframes; ++i)
        data.insert(data.end(), frame.begin(), frame.end());
    return data;
}
