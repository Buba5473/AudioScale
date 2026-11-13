# =============================================================================
#  scale — единая утилита масштабирования MP2 / AC-3 / DTS Core (включая
#  автоматическое извлечение DTS Core из DTS-HD MA/HRA).
#
#  Кроссплатформенная сборка под Windows 10+ (MSVC, MinGW-w64 UCRT64)
#  и Linux 6+.
#
#  Требования:
#    - CMake >= 3.25 (для C++23 и современного target-API);
#    - GCC >= 13, Clang >= 17 или MSVC >= 19.35 (VS 2022 17.5+);
#    - Ninja (рекомендуется) или Makefiles / Visual Studio generator.
#
#  Ключевые возможности:
#    - C++23 (std::expected, std::span, std::jthread, std::format);
#    - единая статическая библиотека scale_common с общим движком;
#    - глубокая оптимизация: LTO, AVX2 baseline, /GL /LTCG, -fno-plt;
#    - опциональный PGO-режим через -DSCALE_PGO=ON;
#    - предупреждения уровня -Wall -Wextra -Wpedantic и /W4 /permissive-;
#    - опциональные unit-тесты через -DSCALE_BUILD_TESTS=ON;
#    - Windows-ресурсы: манифест, VERSIONINFO, иконка (resources/).
# =============================================================================

cmake_minimum_required(VERSION 3.25)

project(scale
    VERSION     2.0.1
    DESCRIPTION "Scaling of MP2, AC-3 and DTS Core audio streams"
    LANGUAGES   CXX
)

# -----------------------------------------------------------------------------
# Глобальные настройки
# -----------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD          23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

# Compiler-version sanity check (must match README requirements).
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
        AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13)
    message(FATAL_ERROR
        "GCC 13+ required (got ${CMAKE_CXX_COMPILER_VERSION}). "
        "std::expected / std::jthread / std::print need GCC 13+.")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang"
        AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 17)
    message(FATAL_ERROR
        "Clang 17+ required (got ${CMAKE_CXX_COMPILER_VERSION}). "
        "std::expected / std::jthread need Clang 17+.")
elseif(MSVC AND MSVC_VERSION LESS 1935)
    message(FATAL_ERROR
        "MSVC 19.35+ (VS 2022 17.5) required "
        "(got ${MSVC_VERSION}).")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # для clangd / cppcheck / clang-tidy

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY
        STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

include(ProcessorCount)
ProcessorCount(SCALE_JOBS)
if(SCALE_JOBS EQUAL 0)
    set(SCALE_JOBS 4)
endif()

# -----------------------------------------------------------------------------
# Опции
# -----------------------------------------------------------------------------

option(SCALE_PGO         "Enable profile-guided optimization"             OFF)
option(SCALE_NATIVE      "Optimize for the current CPU (-march=native)"   OFF)
option(SCALE_WERROR      "Treat warnings as errors"                       OFF)
option(SCALE_BUILD_TESTS "Build unit tests"                               OFF)

# -----------------------------------------------------------------------------
# Зависимости
# -----------------------------------------------------------------------------

find_package(Threads REQUIRED)

# -----------------------------------------------------------------------------
# libstdc++exp: начиная с GCC 14 реализация std::print / std::println
# вынесена в отдельную экспериментальную библиотеку libstdc++exp. При
# статической линковке на MinGW она не подтягивается автоматически, и
# линкер падает с "undefined reference to `std::__open_terminal'".
#
# MSVC использует собственную STL и этой библиотеки не требует. Clang с
# libc++ тоже не требует (и std::print там пока отсутствует — сработает
# fallback в include/scale/print.hpp). Поэтому линкуем только для GNU.
#
# Оформлено как INTERFACE-цель, чтобы автоматически подтягивалось всеми
# исполняемыми файлами проекта без дублирования в каждом target_link_libraries.
# -----------------------------------------------------------------------------

add_library(scale_print_backend INTERFACE)
# GNU и Clang (последний может использовать libstdc++ 14+).
# Если stdc++exp нет при GCC 14+, где __cpp_lib_print определён
# и std::print реально используется (fallback не срабатывает),
# линковка упадёт — это ошибка конфигурации, а не норма.
if((CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
    AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 14)
   OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    find_library(SCALE_STDCXXEXP_LIB stdc++exp)
    if(SCALE_STDCXXEXP_LIB)
        target_link_libraries(scale_print_backend INTERFACE stdc++exp)
    endif()
endif()

# -----------------------------------------------------------------------------
# Общая статическая библиотека: движок + все кодеки.
# -----------------------------------------------------------------------------

add_library(scale_common STATIC
    src/common.cpp
    src/mp2codec.cpp
    src/ac3codec.cpp
    src/dtscodec.cpp
)

target_include_directories(scale_common
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_definitions(scale_common PUBLIC
    SCALE_VERSION="${PROJECT_VERSION}"
)

# scale_print_backend подтянут на уровне scale_common: сама библиотека
# использует scale::print и нуждается в stdc++exp при статической
# линковке. Раньше бэкенд подключался только к scale и scale_tests,
# что оставляло scale_common без нужного символа при внешнем использовании.
target_link_libraries(scale_common
    PUBLIC Threads::Threads
           scale_print_backend
)

# -----------------------------------------------------------------------------
# Единственный исполняемый файл.
# -----------------------------------------------------------------------------

add_executable(scale src/main.cpp)
target_link_libraries(scale PRIVATE scale_common)

# -----------------------------------------------------------------------------
# Общие свойства всех целей.
# -----------------------------------------------------------------------------

foreach(_t IN ITEMS scale_common scale)
    set_target_properties(${_t} PROPERTIES
        CXX_STANDARD          23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )
endforeach()

# -----------------------------------------------------------------------------
# Компилятор-специфичные флаги
# -----------------------------------------------------------------------------

if(WIN32)
    # Windows-специфичные определения — общие для MSVC и MinGW.
    target_compile_definitions(scale_common PUBLIC
        _WIN32_WINNT=0x0A00
        WINVER=0x0A00
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
    )
endif()

if(MSVC)

    target_compile_options(scale_common PUBLIC
        /W4
        /permissive-
        /Zc:__cplusplus
        /Zc:preprocessor
        /Zc:inline
        /EHsc
        /utf-8
        /MP
    )

    target_compile_options(scale_common PUBLIC
        $<$<CONFIG:Release>:
            /O2 /Oi /Ot /Oy /Ob3 /GT /Gw /Gy /GL /fp:fast
        >
    )
    if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND
       CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        target_compile_options(scale_common PUBLIC
            $<$<CONFIG:Release>:/arch:AVX2>
        )
    endif()

    if(SCALE_WERROR)
        target_compile_options(scale_common PRIVATE /WX)
        target_compile_options(scale       PRIVATE /WX)
    endif()

    target_link_options(scale PRIVATE
        $<$<CONFIG:Release>:
            /LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO
        >
    )

    set_property(TARGET scale PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")

else()  # GCC / Clang (Linux, MinGW-w64 UCRT64, macOS)

    # --- общие предупреждения ------------------------------------------------
    target_compile_options(scale_common PUBLIC
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wconversion
        -Wsign-conversion
        -Wcast-qual
        -Wcast-align
        -Wdouble-promotion
        -Wformat=2
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Woverloaded-virtual
        -Wnull-dereference
    )

    # --- предупреждения, поддерживаемые только GCC ---------------------------
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(scale_common PUBLIC
            -Wduplicated-cond
            -Wduplicated-branches
            -Wlogical-op
            -Wuseless-cast
        )
    endif()

    if(SCALE_WERROR)
        target_compile_options(scale_common PRIVATE -Werror)
        target_compile_options(scale       PRIVATE -Werror)
    endif()

    # --- выбор микроархитектуры ----------------------------------------------
    # SCALE_NATIVE=ON  → -march=native -mtune=native;
    # иначе            → x86-64-v3 = AVX2 + FMA + BMI1/2.
    # Baseline совместим со всеми Win 10-совместимыми CPU начиная
    # с Haswell (2013).
    if(SCALE_NATIVE)
        set(SCALE_ARCH_FLAGS -march=native -mtune=native)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        # Baseline AVX2: Intel Haswell (2013) / AMD Excavator (2015).
        set(SCALE_ARCH_FLAGS -march=x86-64-v3 -mtune=generic)
    else()
        # ARM64 и другие архитектуры: -march не задаём.
        set(SCALE_ARCH_FLAGS "")
    endif()

    # --- общие флаги Release для компиляции ----------------------------------
    # PUBLIC: флаги докатываются и до scale (main.cpp), который
    # линкуется с scale_common, иначе main.cpp собирался бы
    # без -O3 / -march.
    target_compile_options(scale_common PUBLIC
        $<$<CONFIG:Release>:
            -O3
            ${SCALE_ARCH_FLAGS}
            -fno-plt
            -fno-semantic-interposition
            -funroll-loops
            -fomit-frame-pointer
            -fstrict-aliasing
            -fmerge-all-constants
            -ffunction-sections
            -fdata-sections
        >
    )

    # --- флаги Release для линковки: общие для ELF и PE ---------------------
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_options(scale PRIVATE
            $<$<CONFIG:Release>:
                -Wl,-O1
                -Wl,--gc-sections
            >
        )
    elseif(APPLE)
        target_link_options(scale PRIVATE
            $<$<CONFIG:Release>:-Wl,-dead_strip>
        )
    endif()

    # --- флаги Release для линковки: только ELF -----------------------------
    # -fuse-linker-plugin, -Wl,--as-needed, -Wl,-z,now и -Wl,-z,relro
    # понимает только GNU ld для ELF. PE-линкер x86_64-w64-mingw32-ld
    # в MSYS2 падает на "unrecognized option '-z'". Поэтому на Windows
    # эти флаги пропускаются.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_options(scale PRIVATE
            $<$<CONFIG:Release>:
                -Wl,--as-needed
                -Wl,-z,now
                -Wl,-z,relro
            >
        )
        # -fuse-linker-plugin — опция драйвера GCC; Clang её
        # не понимает и падает с \"unknown argument\".
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_link_options(scale PRIVATE
                $<$<CONFIG:Release>:-fuse-linker-plugin>
            )
        endif()
    endif()

endif()

# =============================================================================
# Windows-ресурсы: манифест, VERSIONINFO, иконка.
#
# Все три файла лежат в resources/:
#   * scale.rc.in    — шаблон ресурсного файла; CMake подставит версию;
#   * scale.manifest.in — шаблон XML-манифеста; CMake подставит
#                          актуальную версию (DPI, longPathAware,
#                          supportedOS, requestedExecutionLevel);
#   * icon.ico       — иконка приложения (встраивается как RT_GROUP_ICON).
#
# configure_file(@ONLY) генерирует scale.rc в бинарном каталоге с
# абсолютными путями к манифесту и иконке. Forward slashes в путях
# понимают и rc.exe (MSVC), и windres (MinGW). Сгенерированный .rc
# подключается к цели scale; CMake сам вызовет нужный ресурсный
# компилятор и слинкует .res.
#
# Манифест встраивается через .rc (ресурс RT_MANIFEST, ID 1). На MSVC
# это требует отключить автоматическую генерацию манифеста линкером
# (/MANIFEST:NO), иначе возможен конфликт двух RT_MANIFEST.
# =============================================================================

if(WIN32)
    if(MINGW)
        # Отключаем автоматическую генерацию манифеста линкером
        # (CMake 3.30+ игнорирует .manifest для MinGW, вызывая конфликт)
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--disable-auto-import")
        # Флаги для windres: UTF-8 и обход бага с popen
        set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} --use-temp-file -c 65001")
    endif()
    set(SCALE_RESOURCES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources")

    # Проверяем наличие обязательных файлов. Без этой проверки
    # configure_file() сгенерирует .rc с битыми путями, и сборка
    # упадёт с неинформативной ошибкой линкера.
    foreach(_res_file
            "${SCALE_RESOURCES_DIR}/scale.rc.in"
            "${SCALE_RESOURCES_DIR}/scale.manifest.in"
            "${SCALE_RESOURCES_DIR}/icon.ico")
        if(NOT EXISTS "${_res_file}")
            message(FATAL_ERROR
                "Windows-ресурс не найден: ${_res_file}\n"
                "Ожидаются: resources/scale.rc.in, resources/scale.manifest.in, "
                "resources/icon.ico.")
        endif()
    endforeach()

    # @ONLY: подставляются только @VAR@, последовательности ${VAR}
    # остаются нетронутыми. В .rc могут встречаться фрагменты,
    # похожие на ${...}, которые не должны интерпретироваться CMake.
    configure_file(
        "${SCALE_RESOURCES_DIR}/scale.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/scale.rc"
        @ONLY
    )




    # Манифест тоже генерируется из шаблона: версия синхронизирована
    # с project(... VERSION ...). Это устраняет рассинхронизацию
    # между common.hpp / CMakeLists.txt / манифестом.
    configure_file(
        "${SCALE_RESOURCES_DIR}/scale.manifest.in"
        "${CMAKE_CURRENT_BINARY_DIR}/scale.manifest"
        @ONLY
    )

    # Подключаем сгенерированный .rc к цели scale.
    target_sources(scale PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/scale.rc")

    # MSVC: отключаем автогенерацию манифеста линкером, чтобы не
    # было двух RT_MANIFEST (наш из .rc + автоматический).
    if(MSVC)
        target_link_options(scale PRIVATE "/MANIFEST:NO")
    endif()
endif()

# -----------------------------------------------------------------------------
# LTO / IPO
# -----------------------------------------------------------------------------

include(CheckIPOSupported)
check_ipo_supported(RESULT SCALE_IPO_OK OUTPUT SCALE_IPO_MSG)

if(SCALE_IPO_OK AND NOT SCALE_PGO)
    foreach(_t IN ITEMS scale_common scale)
        set_property(TARGET ${_t} PROPERTY
            INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    endforeach()
elseif(SCALE_PGO)
    message(STATUS "IPO/LTO отключено: SCALE_PGO=ON (профиль управляет оптимизацией)")
else()
    message(STATUS "IPO/LTO недоступно: ${SCALE_IPO_MSG}")
endif()

# -----------------------------------------------------------------------------
# PGO полностью управляется скриптами build.sh / build.ps1 через
# CMAKE_CXX_FLAGS / CMAKE_EXE_LINKER_FLAGS. Дублировать флаги здесь
# нельзя: clang получает -fprofile-use=<path> из скрипта, а
# безусловный -fprofile-use без пути переопределил бы его.
# -----------------------------------------------------------------------------

# -----------------------------------------------------------------------------
# Тесты (опционально).
# -----------------------------------------------------------------------------

if(SCALE_BUILD_TESTS)
    enable_testing()

    add_executable(scale_tests
        tests/test_main.cpp
        tests/test_target.cpp
        tests/test_detect.cpp
        tests/test_roundtrip.cpp
        tests/test_args.cpp
        tests/test_extras.cpp
    )
    target_link_libraries(scale_tests PRIVATE scale_common)
    if(SCALE_WERROR)
        if(MSVC)
            target_compile_options(scale_tests PRIVATE /WX)
        else()
            target_compile_options(scale_tests PRIVATE -Werror)
        endif()
    endif()

    add_test(NAME scale_tests COMMAND scale_tests)
endif()

# -----------------------------------------------------------------------------
# Установка
# -----------------------------------------------------------------------------

include(GNUInstallDirs)

install(TARGETS scale
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(TARGETS scale_common
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# -----------------------------------------------------------------------------
# Сводка
# -----------------------------------------------------------------------------

message(STATUS "")
message(STATUS "scale ${PROJECT_VERSION}")
message(STATUS "  Build type    : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler      : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  C++ standard  : ${CMAKE_CXX_STANDARD}")
message(STATUS "  Codecs        : MP2, AC-3, DTS Core (+ DTS-HD core extraction)")
message(STATUS "  PGO enabled   : ${SCALE_PGO}")
message(STATUS "  Native tuning : ${SCALE_NATIVE}")
message(STATUS "  Werror        : ${SCALE_WERROR}")
message(STATUS "  Build tests   : ${SCALE_BUILD_TESTS}")
message(STATUS "  IPO/LTO       : ${SCALE_IPO_OK}")
message(STATUS "  Parallel jobs : ${SCALE_JOBS}")
if(WIN32)
    message(STATUS "  Windows res.  : manifest + VERSIONINFO + icon (resources/)")
endif()
message(STATUS "")
