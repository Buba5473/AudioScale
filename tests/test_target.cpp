/*
   tests/test_target.cpp — unit-тесты для scale::parse_target().
*/

#include "test_common.hpp"
#include "scale/common.hpp"

using namespace scale;
using namespace scale::test;

SCALE_TEST(parse_target_seconds_implicit) {
    auto t = parse_target("20.5");
    SCALE_CHECK(t.has_value());
    SCALE_CHECK(t->kind == TargetSpec::Kind::Seconds);
    SCALE_CHECK_EQ(t->value, 20.5);
}

SCALE_TEST(parse_target_seconds_explicit) {
    auto t = parse_target("20.5s");
    SCALE_CHECK(t.has_value());
    SCALE_CHECK(t->kind == TargetSpec::Kind::Seconds);
    SCALE_CHECK_EQ(t->value, 20.5);
}

SCALE_TEST(parse_target_frames) {
    auto t = parse_target("500f");
    SCALE_CHECK(t.has_value());
    SCALE_CHECK(t->kind == TargetSpec::Kind::Frames);
    SCALE_CHECK_EQ(t->value, 500.0);
}

SCALE_TEST(parse_target_mm_ss) {
    auto t = parse_target("01:30");
    SCALE_CHECK(t.has_value());
    SCALE_CHECK(t->kind == TargetSpec::Kind::Seconds);
    SCALE_CHECK_EQ(t->value, 90.0);
}

SCALE_TEST(parse_target_hh_mm_ss) {
    auto t = parse_target("00:20:30");
    SCALE_CHECK(t.has_value());
    SCALE_CHECK_EQ(t->value, 20.0 * 60.0 + 30.0);
}

SCALE_TEST(parse_target_hh_mm_ss_frac) {
    auto t = parse_target("00:20:30.500");
    SCALE_CHECK(t.has_value());
    SCALE_CHECK_EQ(t->value, 20.0 * 60.0 + 30.5);
}

SCALE_TEST(parse_target_rejects_invalid) {
    SCALE_CHECK(!parse_target("").has_value());
    SCALE_CHECK(!parse_target("abc").has_value());
    SCALE_CHECK(!parse_target("-1").has_value());
    SCALE_CHECK(!parse_target("1:2:3:4").has_value());
    SCALE_CHECK(!parse_target("1:xx").has_value());
}


SCALE_TEST(parse_target_rejects_fractional_frames) {
    SCALE_CHECK(!parse_target("10.5f").has_value());
}

SCALE_TEST(parse_target_rejects_zero) {
    SCALE_CHECK(!parse_target("0").has_value());
    SCALE_CHECK(!parse_target("0f").has_value());
    SCALE_CHECK(!parse_target("0s").has_value());
    SCALE_CHECK(!parse_target("00:00").has_value());
}

SCALE_TEST(parse_target_explicit_seconds_upper) {
    auto t = parse_target("10S");
    SCALE_CHECK(t.has_value());
    SCALE_CHECK(t->kind == TargetSpec::Kind::Seconds);
    SCALE_CHECK_EQ(t->value, 10.0);
}
