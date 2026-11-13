#!/usr/bin/env bash
# =============================================================================
#  build-msys2-ucrt64.sh — сборка scale в MSYS2 UCRT64
#  с полной статической линковкой и глубокой оптимизацией под amd64/Windows 10+.
#
#  Запускать из MSYS2 UCRT64 shell:
#      ./build-msys2-ucrt64.sh
#
#  Требования:
#    - MSYS2 с окружением UCRT64 (переменная MSYSTEM=UCRT64);
#    - mingw-w64-ucrt-x86_64-gcc, mingw-w64-ucrt-x86_64-cmake,
#      mingw-w64-ucrt-x86_64-ninja (устанавливаются автоматически).
# =============================================================================

set -euo pipefail

# --- Разбор аргументов ------------------------------------------------------
BUILD_TESTS=OFF
for arg in "$@"; do
    case "$arg" in
        --tests) BUILD_TESTS=ON ;;
        -h|--help)
            sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "Неизвестный аргумент: $arg (см. --help)" >&2; exit 1 ;;
    esac
done

# --- Проверка окружения MSYS2 UCRT64 ---------------------------------------
if [[ "${MSYSTEM:-}" != "UCRT64" ]]; then
    echo "Ошибка: скрипт нужно запускать из MSYS2 UCRT64 shell." >&2
    echo "Откройте 'MSYS2 UCRT64' из меню Пуск и повторите." >&2
    exit 1
fi

# --- Установка зависимостей через pacman ------------------------------------
# Список пакетов: компилятор, CMake, Ninja. Git не нужен для сборки.
PACMAN_PKGS=(
    mingw-w64-ucrt-x86_64-gcc
    mingw-w64-ucrt-x86_64-cmake
    mingw-w64-ucrt-x86_64-ninja
)

# Проверяем, установлены ли уже нужные пакеты (по наличию cmake и ninja).
if ! command -v cmake >/dev/null 2>&1 \
   || ! command -v ninja >/dev/null 2>&1 \
   || ! command -v g++   >/dev/null 2>&1; then
    echo ">> Устанавливаю зависимости через pacman..."
    pacman -S --needed --noconfirm "${PACMAN_PKGS[@]}"
fi

# --- Параметры сборки --------------------------------------------------------
BUILD_DIR="${BUILD_DIR:-build-ucrt64-static}"
BUILD_TYPE="${BUILD_TYPE:-Release}"

# Флаги архитектуры и Release-профиля задаются в CMakeLists.txt
# (SCALE_NATIVE=OFF → -march=x86-64-v3, SCALE_NATIVE=ON → -march=native).
# Здесь оставляем переменные пустыми, чтобы не дублировать -march/-mtune.
ARCH_FLAGS=""

# Дополнительные флаги компиляции (точечные переопределения).
EXTRA_CXX_FLAGS=""

# Флаги линкера: полная статика + удаление неиспользуемых секций.
# -static заставляет линковать статически libgcc, libstdc++ и winpthread.
# Если в будущем понадобится оставить какую-то системную DLL динамической,
# замените -static на -static-libgcc -static-libstdc++ и добавьте нужные
# исключения.
LINKER_FLAGS="-static -static-libgcc -static-libstdc++ \
-Wl,--gc-sections -Wl,-O1"

# --- Конфигурация и сборка ---------------------------------------------------
echo ">> Конфигурирую CMake (${BUILD_TYPE}) в ${BUILD_DIR}..."
cmake -S . -B "${BUILD_DIR}" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_CXX_FLAGS="${ARCH_FLAGS} ${EXTRA_CXX_FLAGS}" \
    -DCMAKE_EXE_LINKER_FLAGS="${LINKER_FLAGS}" \
    -DSCALE_NATIVE=OFF \
    -DSCALE_WERROR=OFF \
    -DSCALE_BUILD_TESTS="${BUILD_TESTS}"

echo ">> Собираю..."
cmake --build "${BUILD_DIR}" --parallel "$(nproc)"
if [[ "$BUILD_TESTS" == "ON" ]]; then
    echo ">> Запускаю CTest..."
    ctest --test-dir "${BUILD_DIR}" --output-on-failure
fi

echo ">> Готово. Бинарник:"
ls -lh "${BUILD_DIR}/scale.exe" 2>/dev/null || ls -lh "${BUILD_DIR}/scale" 2>/dev/null || echo "   (не найден — проверьте ${BUILD_DIR})"
