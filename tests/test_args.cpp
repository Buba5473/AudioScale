/*
   tests/test_args.cpp — unit-тесты для scale::parse_args() и scale::to_string().

*/

#include "test_common.hpp"
#include "scale/common.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace scale;
using namespace scale::test;


namespace {

// RAII-хелпер: vector<string> → argc/argv.
class Argv {
public:
    explicit Argv(std::vector<std::string> args) : args_(std::move(args)) {
        ptrs_.reserve(args_.size());
        for (auto& s : args_) ptrs_.push_back(s.data());
        ptrs_.push_back(nullptr);
    }
    int    argc() const { return static_cast<int>(args_.size()); }
    char** argv()       { return ptrs_.data(); }
private:
    std::vector<std::string> args_;
    std::vector<char*>       ptrs_;
};

constexpr std::string_view kPrg = "scale";

} // namespace


SCALE_TEST(args_help_short) {
    Argv a({"scale", "-h"});
    auto r = parse_args(a.argc(), a.argv(), kPrg);
    SCALE_CHECK(!r.has_value());
    SCALE_CHECK(r.error() == Error::Help);
}

SCALE_TEST(args_help_long) {
    Argv a({"scale", "--help"});
    auto r = parse_args(a.argc(), a.argv(), kPrg);
    SCALE_CHECK(!r.has_value());
    SCALE_CHECK(r.error() == Error::Help);
}

SCALE_TEST(args_version_short) {
    Argv a({"scale", "-V"});
    auto r = parse_args(a.argc(), a.argv(), kPrg);
    SCALE_CHECK(!r.has_value());
    SCALE_CHECK(r.error() == Error::Version);
}

SCALE_TEST(args_version_long) {
    Argv a({"scale", "--version"});
    auto r = parse_args(a.argc(), a.argv(), kPrg);
    SCALE_CHECK(!r.has_value());
    SCALE_CHECK(r.error() == Error::Version);
}

SCALE_TEST(args_no_input) {
    Argv a({"scale"});
    auto r = parse_args(a.argc(), a.argv(), kPrg);
    SCALE_CHECK(!r.has_value());
    SCALE_CHECK(r.error() == Error::InvalidArgs);
}

SCALE_TEST(args_minimal_infile) {
    Argv a({"scale", "input.mp2"});
    auto r = parse_args(a.argc(), a.argv(), kPrg);
    SCALE_CHECK(r.has_value());
    SCALE_CHECK_EQ(r->infile, std::string("input.mp2"));
    SCALE_CHECK(r->outfile.empty());
    SCALE_CHECK(!r->target.active());
}

SCALE_TEST(args_target_separated) {
    Argv a({"scale", "-t", "20.5", "input.mp2"});
    auto r = parse_args(a.argc(), a.argv(), kPrg);
    SCALE_CHECK(r.has_value());
    SCALE_CHECK_EQ(r->infile, std::string("input.mp2"));
    SCALE_CHECK(r->target.active());
    SCALE_CHECK(r->target.kind == TargetSpec::Kind::Seconds);
    SCALE_CHECK_EQ(r->target.value, 20.5);
}

SCALE_TEST(args_target_glued) {
    Argv a({"scale", "-t20.5", "input.mp2"});
    auto r = parse_args(a.argc(), a.argv(), kPrg);
    SCALE_CHECK(r.has_value());
    SCALE_CHECK(r->target.active());
    SCALE_CHECK_EQ(r->target.value, 20.5);
}

SCALE_TEST(args_target_frames) {
    Argv a({"scale", "-t", "500f", "input.ac3"});
    auto r = parse_args(a.argc(), a.argv(), kPrg);
    SCALE_CHECK(r.has_value());
    SCALE_CHECK(r->target.kind == TargetSpec::Kind::Frames);
    SCALE_CHECK_EQ(r->target.value, 500.0);
}

SCALE_TEST(args_outfile_separated) {
    Argv a({"scale", "-o", "out.mp2", "input.mp2"});
    auto r = parse_args(a.argc(), a.argv(), kPrg);
    SCALE_CHECK(r.has_value());
    SCALE_CHECK_EQ(r->outfile, std::string("out.mp2"));
}

SCALE_TEST(args_outfile_glued) {
    Argv a({"scale", "-oout.mp2", "input.mp2"});
    auto r = parse_args(a.argc(), a.argv(), kPrg);
    SCALE_CHECK(r.has_value());
    SCALE_CHECK_EQ(r->outfile, std::string("out.mp2"));
}

SCALE_TEST(args_target_and_outfile) {
    Argv a({"scale", "-t", "20.5", "-o", "out.mp2", "input.mp2"});
    auto r = parse_args(a.argc(), a.argv(), kPrg);
    SCALE_CHECK(r.has_value());
    SCALE_CHECK_EQ(r->infile,  std::string("input.mp2"));
    SCALE_CHECK_EQ(r->outfile, std::string("out.mp2"));
    SCALE_CHECK_EQ(r->target.value, 20.5);
}

SCALE_TEST(args_bad_target_value) {
    Argv a({"scale", "-t", "abc", "input.mp2"});
    auto r = parse_args(a.argc(), a.argv(), kPrg);
    SCALE_CHECK(!r.has_value());
    SCALE_CHECK(r.error() == Error::InvalidArgs);
}

SCALE_TEST(args_missing_target_value) {
    Argv a({"scale", "-t"});
    auto r = parse_args(a.argc(), a.argv(), kPrg);
    SCALE_CHECK(!r.has_value());
    SCALE_CHECK(r.error() == Error::InvalidArgs);
}

SCALE_TEST(args_unknown_option) {
    Argv a({"scale", "-x", "input.mp2"});
    auto r = parse_args(a.argc(), a.argv(), kPrg);
    SCALE_CHECK(!r.has_value());
    SCALE_CHECK(r.error() == Error::InvalidArgs);
}

SCALE_TEST(args_double_dash_single_infile) {
    Argv a({"scale", "--", "-weird.mp2"});
    auto r = parse_args(a.argc(), a.argv(), kPrg);
    SCALE_CHECK(r.has_value());
    SCALE_CHECK_EQ(r->infile, std::string("-weird.mp2"));
}

SCALE_TEST(args_double_dash_multiple_infiles) {
    Argv a({"scale", "--", "a.mp2", "b.mp2"});
    auto r = parse_args(a.argc(), a.argv(), kPrg);
    SCALE_CHECK(!r.has_value());
    SCALE_CHECK(r.error() == Error::InvalidArgs);
}

SCALE_TEST(args_multiple_infiles) {
    Argv a({"scale", "a.mp2", "b.mp2"});
    auto r = parse_args(a.argc(), a.argv(), kPrg);
    SCALE_CHECK(!r.has_value());
    SCALE_CHECK(r.error() == Error::InvalidArgs);
}

SCALE_TEST(args_stdin_dash) {
    Argv a({"scale", "-"});
    auto r = parse_args(a.argc(), a.argv(), kPrg);
    SCALE_CHECK(!r.has_value());
    SCALE_CHECK(r.error() == Error::InvalidArgs);
}

SCALE_TEST(error_to_string_all_non_empty) {
    for (auto e : { Error::Help, Error::Version, Error::InvalidArgs,
                    Error::FileOpen, Error::FileRead, Error::FileWrite,
                    Error::FileSeek, Error::BadFormat, Error::TooShort }) {
        SCALE_CHECK(!to_string(e).empty());
    }
}
