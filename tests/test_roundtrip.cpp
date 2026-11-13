/*
   tests/test_roundtrip.cpp — интеграционный smoke-тест scale_stream().
*/

#include "test_common.hpp"
#include "scale/common.hpp"

#include <cstdint>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace scale;
using namespace scale::test;


SCALE_TEST(scale_stream_drops_half) {
    namespace fs = std::filesystem;

    const auto dir = fs::temp_directory_path();
    const auto tag = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto in  = dir / ("scale_test_in_"  + tag + ".mp2");
    const auto out = dir / ("scale_test_out_" + tag + ".mp2");

    auto data = make_mp2_stream(100);
    {
        std::ofstream f(in, std::ios::binary);
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
    }

    Args args;
    args.infile  = in.string();
    args.outfile = out.string();
    args.target.kind  = TargetSpec::Kind::Frames;
    args.target.value = 50.0;

    auto r = scale_stream(args);
    SCALE_CHECK(r.has_value());

    std::error_code ec;
    const auto out_size = fs::file_size(out, ec);
    SCALE_CHECK(!ec);
    SCALE_CHECK_EQ(out_size, std::uintmax_t(50 * 576));

    fs::remove(in);
    fs::remove(out);
}


SCALE_TEST(scale_stream_drops_half_ac3) {
    namespace fs = std::filesystem;

    const auto dir = fs::temp_directory_path();
    const auto tag = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto in  = dir / ("scale_test_ac3_in_"  + tag + ".ac3");
    const auto out = dir / ("scale_test_ac3_out_" + tag + ".ac3");

    auto data = make_ac3_stream(100);
    {
        std::ofstream f(in, std::ios::binary);
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
    }

    Args args;
    args.infile  = in.string();
    args.outfile = out.string();
    args.target.kind  = TargetSpec::Kind::Frames;
    args.target.value = 50.0;

    auto r = scale_stream(args);
    SCALE_CHECK(r.has_value());

    std::error_code ec;
    const auto out_size = fs::file_size(out, ec);
    SCALE_CHECK(!ec);
    SCALE_CHECK_EQ(out_size, std::uintmax_t(50 * 768));

    fs::remove(in);
    fs::remove(out);
}


SCALE_TEST(scale_stream_dts_small_file_last_frame) {
    namespace fs = std::filesystem;

    const auto dir = fs::temp_directory_path();
    const auto tag = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto in  = dir / ("scale_test_dts_in_"  + tag + ".dts");
    const auto out = dir / ("scale_test_dts_out_" + tag + ".dts");

    // Поток из 10 DTS Core кадров (960 байт — много меньше kBufSize).
    // Проверяем, что последний кадр не теряется при EOF.
    auto data = make_dts_stream(10);
    {
        std::ofstream f(in, std::ios::binary);
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
    }

    Args args;
    args.infile  = in.string();
    args.outfile = out.string();

    auto r = scale_stream(args);
    SCALE_CHECK(r.has_value());

    std::error_code ec;
    const auto out_size = fs::file_size(out, ec);
    SCALE_CHECK(!ec);
    SCALE_CHECK_EQ(out_size, std::uintmax_t(10 * 96));

    fs::remove(in);
    fs::remove(out);
}
