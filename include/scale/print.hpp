#pragma once
/*
   scale/print.hpp — единый shim для std::print / std::println.

   GCC 13 и Clang 17 (заявленный минимум проекта) не имеют <print> в
   libstdc++/libc++. Чтобы не дублировать fallback в каждом .cpp,
   он вынесен в этот заголовок. Если __cpp_lib_print доступен —
   используем стандартный std::print; иначе — тонкую обёртку над
   std::vformat + std::fwrite.

   Использование:
       #include "scale/print.hpp"
       ...
       scale::print("value = {}\n", v);
       scale::print(stderr, "error: {}\n", msg);
       scale::println("line with newline");

   Все функции принимают std::format-style формат и аргументы.
   scale::print(FILE*, fmt, args...)   — печать в произвольный FILE*
   scale::print(fmt, args...)          — печать в stdout
   scale::println(FILE*, fmt, args...) — то же + '\n'
   scale::println(fmt, args...)        — то же в stdout + '\n'
*/

#include <cstdio>
#include <format>
#include <string_view>
#include <utility>
#include <version>

#if defined(__cpp_lib_print) && __cpp_lib_print >= 202207L

// ---------------------------------------------------------------------------
// Стандартный <print> доступен (GCC 14+, Clang 18+ с libstdc++ 14+,
// MSVC 19.37+ с /std:c++latest или /std:c++23preview).
// ---------------------------------------------------------------------------
#  include <print>

namespace scale {
    using std::print;
    using std::println;
} // namespace scale

#else

// ---------------------------------------------------------------------------
// Fallback: <print> отсутствует. Реализуем через std::vformat + fwrite.
// ---------------------------------------------------------------------------
namespace scale {

// --- print(FILE*, fmt, args...) --------------------------------------------
template <class... Args>
inline void print(std::FILE* f, std::string_view fmt, Args&&... args) {
    auto s = std::vformat(fmt, std::make_format_args(args...));
    std::fwrite(s.data(), 1, s.size(), f);
}

// --- print(fmt, args...) — в stdout ----------------------------------------
template <class... Args>
inline void print(std::string_view fmt, Args&&... args) {
    print(stdout, fmt, std::forward<Args>(args)...);
}

// --- println(FILE*, fmt, args...) ------------------------------------------
template <class... Args>
inline void println(std::FILE* f, std::string_view fmt, Args&&... args) {
    print(f, fmt, std::forward<Args>(args)...);
    std::fputc('\n', f);
}

// --- println(fmt, args...) — в stdout --------------------------------------
template <class... Args>
inline void println(std::string_view fmt, Args&&... args) {
    println(stdout, fmt, std::forward<Args>(args)...);
}

} // namespace scale

#endif // __cpp_lib_print
