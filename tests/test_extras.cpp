/*
   tests/test_extras.cpp — дополнительные регрессионные тесты:
     * single-frame stream + trailing junk (relax-проход);
     * DTS-HD core extraction via frame_span();
     * refuse-overwrite safety in scale_stream();
     * Windows-style /h option (only compiled under _WIN32).
*/

#include "test_common.hpp"
#include "scale/common.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace scale;
using namespace scale::test;

// Single-frame stream: один валидный кадр в начале + хвостовой мусор.
// Строгий проход detect_codec() не срабатывает (нет следующего sync),
// но relax-проход обязан распознать поток.
SCALE_TEST(detect_single_mp2_with_trailing_junk) {
    auto frame = make_mp2_frame();
    std::vector<std::uint8_t> data = frame;
    data.insert(data.end(), 100, 0x55);

    auto d = detect_codec(data);
    SCALE_CHECK(d.has_value());
    SCALE_CHECK_EQ(std::string(d->codec->name()), std::string("mp2"));
    SCALE_CHECK_EQ(d->offset, 0u);
}

SCALE_TEST(detect_single_ac3_with_trailing_junk) {
    auto frame = make_ac3_frame();
    std::vector<std::uint8_t> data = frame;
    data.insert(data.end(), 200, 0xAA);

    auto d = detect_codec(data);
    SCALE_CHECK(d.has_value());
    SCALE_CHECK_EQ(std::string(d->codec->name()), std::string("ac3"));
}

// Relax-проход не должен принимать ложный sync в середине мусора:
// кадр со смещением > 0, за которым нет следующего sync, не считается
// одиночным кадром.
SCALE_TEST(detect_rejects_lone_sync_with_leading_junk) {
    auto frame = make_mp2_frame();
    std::vector<std::uint8_t> data(500, 0x55);
    data.insert(data.end(), frame.begin(), frame.end());
    data.insert(data.end(), 100, 0x55);

    SCALE_CHECK(!detect_codec(data).has_value());
}

// DTS-HD: ядро DTS Core + сигнатура XLL. frame_span() должен перепрыгнуть
// extension и вернуть расстояние до следующего ядра.
SCALE_TEST(dts_frame_span_skips_hd_extension) {
    auto core1 = make_dts_frame();
    auto core2 = make_dts_frame();

    std::vector<std::uint8_t> xll;
    xll.push_back(0x41); xll.push_back(0xA2);
    xll.push_back(0x95); xll.push_back(0x47);
    xll.insert(xll.end(), 60, 0x00);

    std::vector<std::uint8_t> buf;
    buf.insert(buf.end(), core1.begin(), core1.end());
    buf.insert(buf.end(), xll.begin(),   xll.end());
    buf.insert(buf.end(), core2.begin(), core2.end());

    const Codec& c = dts_codec();
    auto fi = c.parse_header(buf);
    SCALE_CHECK(fi.has_value());
    SCALE_CHECK_EQ(fi->framebytes, std::size_t{96});

    const std::size_t span = c.frame_span(buf, *fi);
    SCALE_CHECK_EQ(span, std::size_t{96} + xll.size());
}

// scale_stream() должен отказаться перезаписывать входной файл.
SCALE_TEST(scale_stream_refuses_overwrite_input) {
    namespace fs = std::filesystem;

    const auto dir = fs::temp_directory_path();
    const auto tag = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto path = dir / ("scale_test_self_" + tag + ".mp2");

    auto data = make_mp2_stream(20);
    {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
    }

    Args args;
    args.infile  = path.string();
    args.outfile = path.string();
    args.target.kind  = TargetSpec::Kind::Frames;
    args.target.value = 10.0;

    auto r = scale_stream(args);
    SCALE_CHECK(!r.has_value());
    SCALE_CHECK(r.error() == Error::FileOpen);

    fs::remove(path);
}

#ifdef _WIN32
SCALE_TEST(args_help_windows_slash_h) {
    std::vector<std::string> v{"scale", "/h"};
    std::vector<char*> p;
    for (auto& s : v) p.push_back(s.data());
    p.push_back(nullptr);

    auto r = parse_args(static_cast<int>(v.size()), p.data(), "scale");
    SCALE_CHECK(!r.has_value());
    SCALE_CHECK(r.error() == Error::Help);
}

SCALE_TEST(args_windows_tmp_path_not_option) {
    std::vector<std::string> v{"scale", "/tmp/input.mp2"};
    std::vector<char*> p;
    for (auto& s : v) p.push_back(s.data());
    p.push_back(nullptr);

    auto r = parse_args(static_cast<int>(v.size()), p.data(), "scale");
    SCALE_CHECK(r.has_value());
    SCALE_CHECK_EQ(r->infile, std::string("/tmp/input.mp2"));
}
#endif
