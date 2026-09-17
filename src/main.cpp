/*
   scale — единая утилита масштабирования MP2 / AC-3 / DTS Core.

   Refactored to C++23, 2024. Distributed under GNU GPL v2 or later.

   Единственная точка входа проекта. Формат входного файла определяется
   автоматически (см. scale::detect_codec()):

     * MP2 (MPEG-1 Layer II)                  — масштабируется как есть;
     * AC-3 (Dolby Digital)                   — масштабируется как есть;
     * DTS Core                               — масштабируется как есть;
     * DTS-HD MA / HRA                        — извлекается DTS Core,
                                                extension отбрасывается.

   Использование:
     scale [-t target] [-o outfile] infile
     scale --help

   Формат target (-t):
     20.5        20.5 секунд (по умолчанию секунды);
     20.5s       20.5 секунд (явно);
     500f        500 кадров;
     01:30       1 минута 30 секунд (mm:ss);
     00:20:30    20 минут 30 секунд (hh:mm:ss).

   Без -t выводится только сводка о входном потоке.
   Без -o масштабирование не производится (аналог "dry run").
*/

#include "scale/common.hpp"

#include <expected>
#include <string_view>

namespace {

// ---------------------------------------------------------------------------
// Единая точка выхода для ошибок: печатает "<prg>: <why>" в stderr
// и возвращает код 1. Для Error::Help возвращаем 0, потому что
// пользователь сам попросил справку — это не ошибка.
// ---------------------------------------------------------------------------
[[nodiscard]] int exit_with(std::string_view prg,
                            scale::Error   e,
                            std::string_view why) noexcept {
    if (e == scale::Error::Help) {
        // usage уже напечатан в parse_args(); дополнительных сообщений
        // не требуется, возвращаем успешный код.
        return 0;
    }
    scale::print(stderr, "{}: {}\n", prg, why);
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    // Имя программы для строки использования. Берём из argv[0], отбрасывая
    // путь — это делает вывод аккуратным при вызове "./scale" или "C:\\...\\scale.exe".
    std::string_view prg = (argc > 0 && argv[0] != nullptr) ? argv[0] : "scale";
    if (auto pos = prg.find_last_of("/\\"); pos != std::string_view::npos)
        prg.remove_prefix(pos + 1);
    if (prg.empty()) prg = "scale";

    // -----------------------------------------------------------------------
    // 1. Разбор аргументов.
    //    parse_args() сам печатает usage при -h/--help и при некорректных
    //    опциях; Error::Help возвращает для обеих ситуаций, но мы
    //    различаем их по контексту: при явном запросе справки код выхода 0,
    //    при ошибке аргументов — 1.
    // -----------------------------------------------------------------------
    auto args = scale::parse_args(argc, argv, prg);
    if (!args) {
        const scale::Error e = args.error();
        // Если пользователь явно запросил справку (-h / --help), parse_args
        // возвращает Help, а не InvalidArgs — выходим с кодом 0.
        return exit_with(prg, e, scale::to_string(e));
    }

    // -----------------------------------------------------------------------
    // 2. Полный цикл: детект формата, анализ, план dup/drop,
    //    при необходимости — запись результата в outfile.
    //    Формат входного файла выбирается автоматически; DTS-HD MA/HRA
    //    превращается в DTS Core на лету (см. dtscodec.cpp:frame_span()).
    // -----------------------------------------------------------------------
    auto result = scale::scale_stream(*args);
    if (!result)
        return exit_with(prg, result.error(), scale::to_string(result.error()));

    return 0;
}
