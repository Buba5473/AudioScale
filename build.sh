#!/usr/bin/env bash
# =============================================================================
#  build.sh — кроссплатформенная сборка утилиты scale
#             (MP2 / AC-3 / DTS Core, включая извлечение ядра из DTS-HD MA/HRA).
#
#  Возможности:
#    * выбор генератора (Ninja / Makefiles) и build-type;
#    * автоматическое определение числа ядер;
#    * опциональный PGO-цикл (generate → run → use);
#    * поддержка SCALE_NATIVE / SCALE_WERROR / SCALE_BUILD_TESTS;
#    * чистка build-каталогов и вывод артефактов.
#
#  Использование:
#    ./build.sh                    # Release-сборка в ./build
#    ./build.sh Debug              # Debug-сборка
#    ./build.sh Release --pgo --sample sample.ac3 --target 20.5
#    ./build.sh --clean            # удалить build-каталоги
#    ./build.sh --native           # -march=native
#    ./build.sh --werror           # -Werror
#    ./build.sh --tests            # собрать и запустить unit-тесты
#
#  Переменные окружения:
#    CXX=clang++       — выбор компилятора
#    BUILD_DIR=build   — каталог сборки
#    JOBS=8            — число параллельных задач
# =============================================================================

set -euo pipefail

# -----------------------------------------------------------------------------
# Цветной вывод
# -----------------------------------------------------------------------------
if [[ -t 1 ]] && command -v tput >/dev/null 2>&1 \
   && [[ "$(tput colors 2>/dev/null || echo 0)" -ge 8 ]]; then
    C_RESET="$(tput sgr0)"
    C_BOLD="$(tput bold)"
    C_RED="$(tput setaf 1)"
    C_GREEN="$(tput setaf 2)"
    C_YELLOW="$(tput setaf 3)"
    C_CYAN="$(tput setaf 6)"
else
    C_RESET="" C_BOLD="" C_RED="" C_GREEN="" C_YELLOW="" C_CYAN=""
fi

log()   { printf '%s>>%s %s\n'  "${C_CYAN}${C_BOLD}"   "${C_RESET}" "$*"; }
ok()    { printf '%s✓%s  %s\n'  "${C_GREEN}${C_BOLD}"  "${C_RESET}" "$*"; }
warn()  { printf '%s!%s  %s\n'  "${C_YELLOW}${C_BOLD}" "${C_RESET}" "$*" >&2; }
err()   { printf '%s✗%s  %s\n'  "${C_RED}${C_BOLD}"    "${C_RESET}" "$*" >&2; }
die()   { err "$*"; exit 1; }

# -----------------------------------------------------------------------------
# Единая точка правды для артефактов
# -----------------------------------------------------------------------------
MAIN_EXE="scale"

# -----------------------------------------------------------------------------
# Разбор аргументов
# -----------------------------------------------------------------------------
BUILD_TYPE="Release"
BUILD_DIR="${BUILD_DIR:-build}"
PGO_DIR="${PGO_DIR:-build-pgo}"
PGO=0
CLEAN=0
NATIVE=0
WERROR=0
TESTS=0
GENERATOR=""
SAMPLE_INPUT="${SAMPLE_INPUT:-}"
SAMPLE_TARGET="${SAMPLE_TARGET:-10.0}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        Debug|Release|RelWithDebInfo|MinSizeRel)
            BUILD_TYPE="$1"; shift ;;
        --pgo)        PGO=1; shift ;;
        --clean)      CLEAN=1; shift ;;
        --native)     NATIVE=1; shift ;;
        --werror)     WERROR=1; shift ;;
        --tests)      TESTS=1; shift ;;
        --ninja)      GENERATOR="Ninja"; shift ;;
        --make)       GENERATOR="Unix Makefiles"; shift ;;
        --build-dir)  BUILD_DIR="$2"; shift 2 ;;
        --sample)     SAMPLE_INPUT="$2"; shift 2 ;;
        --target)     SAMPLE_TARGET="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) die "Неизвестный аргумент: $1 (см. --help)" ;;
    esac
done

# -----------------------------------------------------------------------------
# Чистка
# -----------------------------------------------------------------------------
if [[ "$CLEAN" -eq 1 ]]; then
    log "Удаляю каталоги сборки: ${BUILD_DIR}, ${PGO_DIR}"
    rm -rf -- "$BUILD_DIR" "$PGO_DIR"
    ok "Готово"
    exit 0
fi

# -----------------------------------------------------------------------------
# Определение числа параллельных задач
# -----------------------------------------------------------------------------
if [[ -z "${JOBS:-}" ]]; then
    if command -v nproc >/dev/null 2>&1; then
        JOBS="$(nproc)"
    elif command -v sysctl >/dev/null 2>&1; then
        JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
    elif command -v getconf >/dev/null 2>&1; then
        JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
    else
        JOBS=4
    fi
fi

# -----------------------------------------------------------------------------
# Проверка инструментов
# -----------------------------------------------------------------------------
command -v cmake >/dev/null 2>&1 || die "cmake не найден в PATH"
CMAKE_VERSION="$(cmake --version | head -n1 | awk '{print $3}')"
log "cmake ${CMAKE_VERSION}"

CXX_BIN="${CXX:-c++}"
command -v "$CXX_BIN" >/dev/null 2>&1 || die "C++ компилятор '$CXX_BIN' не найден"
CXX_VERSION="$("$CXX_BIN" --version | head -n1)"
log "C++ compiler: ${CXX_VERSION}"

# Автовыбор генератора, если не задан явно.
if [[ -z "$GENERATOR" ]]; then
    if command -v ninja >/dev/null 2>&1; then
        GENERATOR="Ninja"
    else
        GENERATOR="Unix Makefiles"
    fi
fi
log "generator: ${GENERATOR}"

# -----------------------------------------------------------------------------
# Общие cmake-флаги
# -----------------------------------------------------------------------------
COMMON_FLAGS=(
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
    -DCMAKE_CXX_COMPILER="${CXX_BIN}"
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    -DSCALE_NATIVE="$([[ "$NATIVE" -eq 1 ]] && echo ON || echo OFF)"
    -DSCALE_WERROR="$([[ "$WERROR" -eq 1 ]] && echo ON || echo OFF)"
    -DSCALE_BUILD_TESTS="$([[ "$TESTS" -eq 1 ]] && echo ON || echo OFF)"
)

# -----------------------------------------------------------------------------
# Печать артефактов: ищет exe во всех возможных местах
# -----------------------------------------------------------------------------
print_artifacts() {
    local base="$1"
    local cfg="$2"
    local label="${3:-Артефакты}"
    echo
    log "${label}:"
    local cand
    for cand in \
        "${base}/${MAIN_EXE}" \
        "${base}/${cfg}/${MAIN_EXE}" \
        "${base}/bin/${MAIN_EXE}" \
        "${base}/bin/${cfg}/${MAIN_EXE}"; do
        if [[ -x "$cand" ]]; then
            local size
            size="$(du -h "$cand" | cut -f1)"
            printf '  %s%-20s%s %s  %s\n' \
                "$C_BOLD" "$MAIN_EXE" "$C_RESET" "$size" "$cand"
            return
        fi
    done
    warn "Не найден бинарник ${MAIN_EXE}"
}

# =============================================================================
# Обычная (не-PGO) сборка
# =============================================================================
if [[ "$PGO" -eq 0 ]]; then
    log "Конфигурирую ${BUILD_TYPE} в ${BUILD_DIR} (jobs=${JOBS})"
    cmake -S . -B "$BUILD_DIR" -G "$GENERATOR" "${COMMON_FLAGS[@]}"

    log "Собираю"
    cmake --build "$BUILD_DIR" --parallel "$JOBS"

    ok "Сборка завершена"

    print_artifacts "$BUILD_DIR" "$BUILD_TYPE"

    # -------------------------------------------------------------------------
    # Быстрая самопроверка (если запрошены тесты)
    # -------------------------------------------------------------------------
    if [[ "$TESTS" -eq 1 ]]; then
        log "Запускаю CTest"
        ctest --test-dir "$BUILD_DIR" --output-on-failure --parallel "$JOBS"
        ok "Тесты прошли"
    fi

    exit 0
fi

# =============================================================================
# PGO-цикл: generate → run → use
# =============================================================================
log "PGO-режим включён"
[[ -n "$SAMPLE_INPUT" ]] || die "Для PGO укажите --sample <файл.mp2|файл.ac3|файл.dts>"
[[ -f "$SAMPLE_INPUT" ]]  || die "Sample-файл не найден: $SAMPLE_INPUT"

log "Тренировочный бинарник: ${MAIN_EXE} (формат определяется автоматически)"

# --- Шаг 1: сборка с инструментированием -------------------------------------
log "[PGO 1/3] Сборка с -fprofile-generate в ${PGO_DIR}"
if [[ "$CXX_BIN" == *clang* ]]; then
    PGO_GEN_FLAGS=(-fprofile-generate="${PWD}/${PGO_DIR}/prof")
    PGO_GEN_LDFLAGS=(-fprofile-generate="${PWD}/${PGO_DIR}/prof")
else
    PGO_GEN_FLAGS=(-fprofile-generate)
    PGO_GEN_LDFLAGS=(-fprofile-generate)
fi

cmake -S . -B "$PGO_DIR" -G "$GENERATOR" \
    "${COMMON_FLAGS[@]}" \
    -DCMAKE_CXX_FLAGS="${PGO_GEN_FLAGS[*]}" \
    -DCMAKE_EXE_LINKER_FLAGS="${PGO_GEN_LDFLAGS[*]}" \
    -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF

cmake --build "$PGO_DIR" --parallel "$JOBS"

# --- Шаг 2: прогон на sample-файле -------------------------------------------
log "[PGO 2/3] Прогон на ${SAMPLE_INPUT} (target=${SAMPLE_TARGET}s)"
SAMPLE_OUT="$(mktemp -t pgo_XXXXXX.out)"
trap 'rm -f "$SAMPLE_OUT"' EXIT

run_train() {
    local path="$PGO_DIR/$MAIN_EXE"
    [[ -x "$path" ]] || path="$PGO_DIR/$BUILD_TYPE/$MAIN_EXE"
    [[ -x "$path" ]] || die "Не найден тренировочный бинарник: $MAIN_EXE"

    # Несколько прогонов — стабильнее профиль.
    local i
    for i in 1 2 3; do
        "$path" -t "$SAMPLE_TARGET" -o "$SAMPLE_OUT" "$SAMPLE_INPUT" >/dev/null
    done
    # Прогон без -o, чтобы покрыть ветку «только отчёт».
    "$path" "$SAMPLE_INPUT" >/dev/null
    ok "Профиль для $MAIN_EXE собран"
}

run_train

# --- Шаг 3: финальная сборка с использованием профиля ------------------------
log "[PGO 3/3] Финальная сборка с -fprofile-use в ${BUILD_DIR}"
if [[ "$CXX_BIN" == *clang* ]]; then
    PGO_USE_FLAGS=(-fprofile-use="${PWD}/${PGO_DIR}/prof" -fprofile-correction)
    PGO_USE_LDFLAGS=(-fprofile-use="${PWD}/${PGO_DIR}/prof")
else
    PGO_USE_FLAGS=(-fprofile-use -fprofile-correction -Wno-missing-profile)
    PGO_USE_LDFLAGS=(-fprofile-use)
fi

cmake -S . -B "$BUILD_DIR" -G "$GENERATOR" \
    "${COMMON_FLAGS[@]}" \
    -DSCALE_PGO=ON \
    -DCMAKE_CXX_FLAGS="${PGO_USE_FLAGS[*]}" \
    -DCMAKE_EXE_LINKER_FLAGS="${PGO_USE_LDFLAGS[*]}"

cmake --build "$BUILD_DIR" --parallel "$JOBS"

ok "PGO-сборка завершена"

print_artifacts "$BUILD_DIR" "$BUILD_TYPE" "Артефакты (PGO)"

# -----------------------------------------------------------------------------
# Опциональное сравнение с baseline (BENCH=1)
# -----------------------------------------------------------------------------
if [[ -n "${BENCH:-}" ]]; then
    log "Сравнение (упрощённое) времени работы:"
    local_pgo="${BUILD_DIR}/${MAIN_EXE}"
    local_base="build-baseline/${MAIN_EXE}"
    if [[ -x "$local_base" && -x "$local_pgo" ]]; then
        t1=$( { /usr/bin/time -f '%e' "$local_base" "$SAMPLE_INPUT" >/dev/null; } 2>&1 || true )
        t2=$( { /usr/bin/time -f '%e' "$local_pgo"  "$SAMPLE_INPUT" >/dev/null; } 2>&1 || true )
        printf '  %-10s baseline=%ss  pgo=%ss\n' "$MAIN_EXE" "$t1" "$t2"
    else
        warn "Для BENCH нужны и build-baseline/, и ${BUILD_DIR}/"
    fi
fi

exit 0
