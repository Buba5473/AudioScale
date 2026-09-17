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
#    - опциональные unit-тесты через -DSCALE_BUILD_TESTS=ON.
# =============================================================================

cmake_minimum_required(VERSION 3.25)

project(scale
    VERSION     2.0.0
    DESCRIPTION "Scaling of MP2, AC-3 and DTS Core audio streams"
    LANGUAGES   CXX
)

# -----------------------------------------------------------------------------
# Глобальные настройки
# -----------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD          23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

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
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_libraries(scale_print_backend INTERFACE stdc++exp)
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

target_link_libraries(scale_common
    PUBLIC Threads::Threads
)

# -----------------------------------------------------------------------------
# Единственный исполняемый файл.
# -----------------------------------------------------------------------------

add_executable(scale src/main.cpp)
target_link_libraries(scale PRIVATE scale_common scale_print_backend)

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

if(MSVC)

    target_compile_definitions(scale_common PUBLIC
        _WIN32_WINNT=0x0A00
        WINVER=0x0A00
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
    )

    target_compile_options(scale_common PRIVATE
        /W4
        /permissive-
        /Zc:__cplusplus
        /Zc:preprocessor
        /Zc:inline
        /EHsc
        /utf-8
        /MP
    )

    target_compile_options(scale_common PRIVATE
        $<$<CONFIG:Release>:
            /O2 /Oi /Ot /Oy /Ob3 /GT /Gw /Gy /GL /fp:fast /arch:AVX2
        >
    )

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
    target_compile_options(scale_common PRIVATE
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
        target_compile_options(scale_common PRIVATE
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
    else()
        set(SCALE_ARCH_FLAGS -march=x86-64-v3 -mtune=generic)
    endif()

    # --- общие флаги Release для компиляции ----------------------------------
    target_compile_options(scale_common PRIVATE
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
    target_link_options(scale PRIVATE
        $<$<CONFIG:Release>:
            -Wl,-O1
            -Wl,--gc-sections
        >
    )

    # --- флаги Release для линковки: только ELF -----------------------------
    # -fuse-linker-plugin, -Wl,--as-needed, -Wl,-z,now и -Wl,-z,relro
    # понимает только GNU ld для ELF. PE-линкер x86_64-w64-mingw32-ld
    # в MSYS2 падает на "unrecognized option '-z'". Поэтому на Windows
    # эти флаги пропускаются.
    if(NOT WIN32)
        target_link_options(scale PRIVATE
            $<$<CONFIG:Release>:
                -fuse-linker-plugin
                -Wl,--as-needed
                -Wl,-z,now
                -Wl,-z,relro
            >
        )
    endif()

endif()

# -----------------------------------------------------------------------------
# LTO / IPO
# -----------------------------------------------------------------------------

include(CheckIPOSupported)
check_ipo_supported(RESULT SCALE_IPO_OK OUTPUT SCALE_IPO_MSG)

if(SCALE_IPO_OK)
    foreach(_t IN ITEMS scale_common scale)
        set_property(TARGET ${_t} PROPERTY
            INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    endforeach()
else()
    message(STATUS "IPO/LTO недоступно: ${SCALE_IPO_MSG}")
endif()

# -----------------------------------------------------------------------------
# PGO (когда включён -DSCALE_PGO=ON).
# Скрипты build.sh / build.ps1 уже передают конкретные PGO-флаги через
# CMAKE_CXX_FLAGS / CMAKE_EXE_LINKER_FLAGS; здесь только страховка.
# -----------------------------------------------------------------------------

if(SCALE_PGO AND NOT MSVC)
    target_compile_options(scale_common PRIVATE
        -fprofile-use
        -fprofile-correction
        -Wno-missing-profile
    )
    target_link_options(scale PRIVATE -fprofile-use)
endif()

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
    )
    target_link_libraries(scale_tests PRIVATE scale_common scale_print_backend)

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
message(STATUS "")
