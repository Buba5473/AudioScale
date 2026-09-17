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
if ! command -v cmake >/dev/null 2>&1 || ! command -v ninja >/dev/null 2>&1; then
    echo ">> Устанавливаю зависимости через pacman..."
    pacman -S --needed --noconfirm "${PACMAN_PKGS[@]}"
fi

# --- Параметры сборки --------------------------------------------------------
BUILD_DIR="${BUILD_DIR:-build-ucrt64-static}"
BUILD_TYPE="${BUILD_TYPE:-Release}"

# Глубокая оптимизация под amd64 (x86-64-v3 = AVX2 + FMA + BMI1/2).
# Если нужна полная нативная оптимизация — замените на -march=native.
ARCH_FLAGS="-march=x86-64-v3 -mtune=generic"

# Дополнительные флаги компиляции, помимо заданных в CMakeLists.txt.
# CMakeLists.txt уже добавляет -O3, -flto, -fno-plt и т.д. для Release,
# поэтому здесь дублировать их не нужно. Этот массив можно использовать
# для точечных переопределений.
EXTRA_CXX_FLAGS=""

# Флаги линкера: полная статика + удаление неиспользуемых секций.
# -static заставляет линковать статически libgcc, libstdc++ и winpthread.
# Если в будущем понадобится оставить какую-то системную DLL динамической,
# замените -static на -static-libgcc -static-libstdc++ и добавьте нужные
# исключения.
LINKER_FLAGS="-static -static-libgcc -static-libstdc++ \
-Wl,--gc-sections -Wl,-O1 -Wl,--as-needed"

# --- Конфигурация и сборка ---------------------------------------------------
echo ">> Конфигурирую CMake (${BUILD_TYPE}) в ${BUILD_DIR}..."
cmake -S . -B "${BUILD_DIR}" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_CXX_FLAGS="${ARCH_FLAGS} ${EXTRA_CXX_FLAGS}" \
    -DCMAKE_EXE_LINKER_FLAGS="${LINKER_FLAGS}" \
    -DSCALE_NATIVE=OFF \
    -DSCALE_WERROR=OFF \
    -DSCALE_BUILD_TESTS=OFF

echo ">> Собираю..."
cmake --build "${BUILD_DIR}" --parallel "$(nproc)"

echo ">> Готово. Бинарник:"
ls -lh "${BUILD_DIR}/scale.exe" 2>/dev/null || ls -lh "${BUILD_DIR}/scale" 2>/dev/null || echo "   (не найден — проверьте ${BUILD_DIR})"
