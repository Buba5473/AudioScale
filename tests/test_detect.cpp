/*
   tests/test_detect.cpp — unit-тесты для scale::detect_codec().
*/

#include "test_common.hpp"
#include "scale/common.hpp"

#include <cstdint>
#include <string>
#include <vector>

using namespace scale;
using namespace scale::test;

SCALE_TEST(detect_mp2_minimal) {
    auto data = make_mp2_stream(2);

    auto d = detect_codec(data);
    SCALE_CHECK(d.has_value());
    SCALE_CHECK_EQ(std::string(d->codec->name()), std::string("mp2"));
    SCALE_CHECK_EQ(d->offset, 0u);
    SCALE_CHECK_EQ(d->fi.framebytes, std::size_t{576});
}

SCALE_TEST(detect_mp2_with_leading_junk) {
    auto frame = make_mp2_frame();
    std::vector<std::uint8_t> data(7, 0x00);
    data.insert(data.end(), frame.begin(), frame.end());
    data.insert(data.end(), frame.begin(), frame.end());

    auto d = detect_codec(data);
    SCALE_CHECK(d.has_value());
    SCALE_CHECK_EQ(std::string(d->codec->name()), std::string("mp2"));
    SCALE_CHECK_EQ(d->offset, 7u);
}

SCALE_TEST(detect_ac3_minimal) {
    auto data = make_ac3_stream(2);

    auto d = detect_codec(data);
    SCALE_CHECK(d.has_value());
    SCALE_CHECK_EQ(std::string(d->codec->name()), std::string("ac3"));
    SCALE_CHECK_EQ(d->offset, 0u);
    SCALE_CHECK_EQ(d->fi.framebytes, std::size_t{768});
    SCALE_CHECK_EQ(d->fi.bitrate_kbps, std::size_t{192});
    SCALE_CHECK_EQ(d->fi.freq_khz, 48.0);
}

SCALE_TEST(detect_dts_minimal) {
    auto data = make_dts_stream(2);

    auto d = detect_codec(data);
    SCALE_CHECK(d.has_value());
    SCALE_CHECK_EQ(std::string(d->codec->name()), std::string("dts"));
    SCALE_CHECK_EQ(d->offset, 0u);
    SCALE_CHECK_EQ(d->fi.framebytes, std::size_t{96});
    SCALE_CHECK_EQ(d->fi.bitrate_kbps, std::size_t{1536});
}

SCALE_TEST(detect_rejects_garbage) {
    std::vector<std::uint8_t> data(1024, 0x55);
    SCALE_CHECK(!detect_codec(data).has_value());
}

SCALE_TEST(detect_rejects_short_input) {
    std::vector<std::uint8_t> data{0xFF, 0xFD};
    SCALE_CHECK(!detect_codec(data).has_value());
}
