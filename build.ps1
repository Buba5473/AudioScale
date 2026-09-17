<#
.SYNOPSIS
    Кроссплатформенная сборка утилиты scale под Windows 10+.

.DESCRIPTION
    Обёртка над CMake, повторяющая возможности build.sh:
      * выбор генератора (Ninja / Visual Studio 2022);
      * выбор архитектуры (x64 / arm64 / Win32);
      * автоматическое определение числа ядер;
      * опциональный PGO-цикл (generate → run → use) для MSVC и clang-cl;
      * поддержка SCALE_NATIVE / SCALE_WERROR / SCALE_BUILD_TESTS;
      * чистка build-каталогов и вывод артефактов.

    Входной формат (MP2 / AC-3 / DTS Core / DTS-HD MA/HRA) определяется
    единственным бинарником scale автоматически, поэтому PGO-тренировка
    выполняется одним исполняемым файлом.

.PARAMETER Configuration
    Release | RelWithDebInfo | Debug | MinSizeRel. По умолчанию Release.

.PARAMETER Ninja
    Использовать генератор Ninja вместо Visual Studio. Требует,
    чтобы cl.exe или clang-cl.exe были доступны в PATH — запускайте
    скрипт из "x64 Native Tools Command Prompt for VS 2022",
    либо добавьте MSVC в PATH вручную.

.PARAMETER Arch
    x64 | arm64 | Win32. Игнорируется при -Ninja.

.PARAMETER Pgo
    Включить PGO-цикл. Требует -Sample.

.PARAMETER Sample
    Путь к тренировочному аудиофайлу (.mp2 / .ac3 / .dts / .dtshd).

.PARAMETER Target
    Целевая длительность в секундах для PGO-прогона. По умолчанию 10.

.PARAMETER Native
    Собрать с оптимизацией под текущий CPU (аналог -march=native).
    Для MSVC маппится в /arch:AVX2 (у MSVC нет прямого аналога).

.PARAMETER Werror
    Считать предупреждения ошибками (/WX).

.PARAMETER Tests
    Собрать и запустить CTest-тесты.

.PARAMETER Clean
    Удалить каталоги сборки и выйти.

.PARAMETER BuildDir
    Каталог сборки. По умолчанию "build".

.EXAMPLE
    .\build.ps1
    .\build.ps1 -Configuration Debug -Ninja
    .\build.ps1 -Pgo -Sample .\sample.dts -Target 20.5
    .\build.ps1 -Tests
    .\build.ps1 -Clean
#>

[CmdletBinding()]
param(
    [ValidateSet("Debug","Release","RelWithDebInfo","MinSizeRel")]
    [string] $Configuration = "Release",

    [switch] $Ninja,
    [switch] $Pgo,
    [switch] $Native,
    [switch] $Werror,
    [switch] $Tests,
    [switch] $Clean,

    [ValidateSet("x64","arm64","Win32")]
    [string] $Arch = "x64",

    [string] $Sample   = "",
    [double] $Target   = 10.0,
    [string] $BuildDir = "build"
)

# -----------------------------------------------------------------------------
# Строгий режим и остановка на первой ошибке
# -----------------------------------------------------------------------------
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# -----------------------------------------------------------------------------
# Единая точка правды: имя единственного бинарника проекта
# -----------------------------------------------------------------------------
$MainExe = "scale"

# -----------------------------------------------------------------------------
# Цветной вывод
# -----------------------------------------------------------------------------
function Write-Log     ($m) { Write-Host ">> $m"  -ForegroundColor Cyan }
function Write-Ok      ($m) { Write-Host "OK $m"  -ForegroundColor Green }
function Write-WarnMsg ($m) { Write-Warning  $m }
function Write-Err     ($m) { Write-Host "!! $m"  -ForegroundColor Red }

function Die ($m) {
    Write-Err $m
    exit 1
}

# -----------------------------------------------------------------------------
# Проверка окружения
# -----------------------------------------------------------------------------
function Test-Command ($name) {
    return [bool](Get-Command $name -ErrorAction SilentlyContinue)
}

if (-not (Test-Command "cmake")) {
    Die "cmake не найден в PATH. Установите CMake 3.25+ или добавьте в PATH."
}

$cmakeVersion = (& cmake --version | Select-Object -First 1) -replace '^cmake version\s+',''
Write-Log "cmake $cmakeVersion"

# Проверка ОС: для _WIN32_WINNT=0x0A00 нужна Windows 10+.
$osVer = [System.Environment]::OSVersion.Version
if ($osVer.Major -lt 10) {
    Write-WarnMsg "Обнаружена ОС ниже Windows 10 ($($osVer)). Сборка продолжится, но _WIN32_WINNT=0x0A00 может быть некорректен."
}

# -----------------------------------------------------------------------------
# Чистка
# -----------------------------------------------------------------------------
if ($Clean) {
    Write-Log "Удаляю каталоги сборки: $BuildDir, build-pgo"
    foreach ($d in @($BuildDir, "build-pgo")) {
        if (Test-Path $d) {
            Remove-Item -Recurse -Force -LiteralPath $d
        }
    }
    Write-Ok "Готово"
    exit 0
}

# -----------------------------------------------------------------------------
# Число параллельных задач
# -----------------------------------------------------------------------------
if (-not $env:JOBS) {
    $Jobs = [Environment]::ProcessorCount
    if ($Jobs -le 0) { $Jobs = 4 }
} else {
    $Jobs = [int] $env:JOBS
}
Write-Log "jobs=$Jobs"

# -----------------------------------------------------------------------------
# Выбор генератора
# -----------------------------------------------------------------------------
if ($Ninja) {
    if (-not (Test-Command "ninja")) {
        Die "ninja не найден в PATH. Установите Ninja или используйте VS-генератор."
    }
    $Generator     = "Ninja"
    $IsMultiConfig = $false
} else {
    # Ищем VS 2022; при отсутствии — падаем с понятным сообщением.
    $vsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    $vsPath  = $null
    if (Test-Path $vsWhere) {
        $vsPath = & $vsWhere -latest -products * `
                             -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                             -property installationPath 2>$null
    }
    if (-not $vsPath) {
        Die "Visual Studio 2022 с C++ toolchain не найден. Установите VS 2022 или используйте -Ninja."
    }
    $Generator     = "Visual Studio 17 2022"
    $IsMultiConfig = $true
}
Write-Log "generator: $Generator"

# -----------------------------------------------------------------------------
# Общие cmake-флаги
# -----------------------------------------------------------------------------
$commonArgs = @(
    "-S", ".",
    "-B", $BuildDir,
    "-G", $Generator,
    "-DCMAKE_BUILD_TYPE=$Configuration",
    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
    "-DSCALE_NATIVE=$(if ($Native) {'ON'} else {'OFF'})",
    "-DSCALE_WERROR=$(if ($Werror) {'ON'} else {'OFF'})",
    "-DSCALE_BUILD_TESTS=$(if ($Tests)  {'ON'} else {'OFF'})"
)

if (-not $Ninja) {
    $commonArgs += @("-A", $Arch)
}

# -----------------------------------------------------------------------------
# Поиск собранного exe с учётом multi-config
# -----------------------------------------------------------------------------
function Get-ExePath ([string]$base, [string]$cfg, [string]$name) {
    $candidates = @(
        (Join-Path $base "$name.exe"),
        (Join-Path $base "$cfg\$name.exe"),
        (Join-Path $base "bin\$name.exe"),
        (Join-Path $base "bin\$cfg\$name.exe")
    )
    foreach ($c in $candidates) {
        if (Test-Path $c -PathType Leaf) { return $c }
    }
    return $null
}

# -----------------------------------------------------------------------------
# Печать артефактов
# -----------------------------------------------------------------------------
function Write-Artifacts ([string]$base, [string]$cfg, [string]$label = "Артефакты") {
    Write-Host ""
    Write-Log "${label}:"
    $p = Get-ExePath -base $base -cfg $cfg -name $MainExe
    if ($p) {
        $size = "{0:N0} KiB" -f ((Get-Item $p).Length / 1KB)
        Write-Host ("  {0,-12} {1,10}  {2}" -f $MainExe, $size, $p)
    } else {
        Write-WarnMsg "Не найден бинарник $MainExe"
    }
}

# =============================================================================
# Обычная (не-PGO) сборка
# =============================================================================
if (-not $Pgo) {
    Write-Log "Конфигурирую $Configuration ($Arch) в $BuildDir"
    & cmake @commonArgs
    if ($LASTEXITCODE -ne 0) { Die "cmake configure не удался" }

    Write-Log "Собираю (parallel=$Jobs)"
    & cmake --build $BuildDir --config $Configuration --parallel $Jobs
    if ($LASTEXITCODE -ne 0) { Die "cmake build не удался" }

    Write-Ok "Сборка завершена"

    Write-Artifacts -base $BuildDir -cfg $Configuration

    if ($Tests) {
        Write-Log "Запускаю CTest"
        $ctestArgs = @("--test-dir", $BuildDir, "--output-on-failure",
                       "--parallel", "$Jobs")
        if ($IsMultiConfig) { $ctestArgs += @("-C", $Configuration) }
        & ctest @ctestArgs
        if ($LASTEXITCODE -ne 0) { Die "Тесты упали" }
        Write-Ok "Тесты прошли"
    }

    exit 0
}

# =============================================================================
# PGO-цикл: generate → run → use
# =============================================================================
Write-Log "PGO-режим включён"

if ([string]::IsNullOrWhiteSpace($Sample)) {
    Die "Для PGO укажите -Sample <файл.mp2|файл.ac3|файл.dts|файл.dtshd>"
}
if (-not (Test-Path $Sample -PathType Leaf)) {
    Die "Sample-файл не найден: $Sample"
}
$Sample = (Resolve-Path $Sample).Path

# Формат определяется автоматически единственным бинарником scale.
Write-Log "Тренировочный бинарник: $MainExe (формат определится автоматически)"

$pgoDir  = "build-pgo"
$profDir = Join-Path $pgoDir "prof"
if (-not (Test-Path $profDir)) { New-Item -ItemType Directory -Path $profDir | Out-Null }
$profDirFull = (Resolve-Path $profDir).Path

# -----------------------------------------------------------------------------
# Определяем инструмент компиляции: MSVC cl.exe или clang-cl.exe.
# -----------------------------------------------------------------------------
$useClangCl = $false
if (Test-Command "clang-cl") {
    $cxxEnv = (& clang-cl --version 2>$null) -join "`n"
    if ($cxxEnv -match "clang") { $useClangCl = $true }
}

# -----------------------------------------------------------------------------
# Формируем флаги PGO.
#
# ВАЖНО: для MSVC /GL — это флаг КОМПИЛЯТОРА, а /LTCG:PGI и /LTCG:PGO —
# флаги ЛИНКЕРА. Раньше /LTCG попадал в CMAKE_CXX_FLAGS, что вызывало
# ошибку cl.exe. Теперь они разделены: compileFlags и linkerFlags.
# -----------------------------------------------------------------------------
if ($useClangCl) {
    Write-Log "PGO через clang-cl (-fprofile-generate / -fprofile-use)"

    $genCompileFlags  = "-fprofile-generate=`"$profDirFull`""
    $genLinkerFlags   = "-fprofile-generate=`"$profDirFull`""

    $useCompileFlags  = "-fprofile-use=`"$profDirFull`" -fprofile-correction"
    $useLinkerFlags   = "-fprofile-use=`"$profDirFull`""
} else {
    Write-Log "PGO через MSVC (/GL → прогон → /LTCG:PGO)"

    # MSVC PGO использует переменную окружения VCPROFILE_PATH —
    # куда складывать .pgc/.pgd.
    $env:VCPROFILE_PATH = $profDirFull

    $genCompileFlags  = "/GL"
    $genLinkerFlags   = "/LTCG:PGI"

    $useCompileFlags  = "/GL"
    $useLinkerFlags   = "/LTCG:PGO"
}

# --- Шаг 1: сборка с инструментированием -------------------------------------
Write-Log "[PGO 1/3] Сборка с инструментированием в $pgoDir"
$genArgs = @(
    "-S", ".", "-B", $pgoDir, "-G", $Generator,
    "-DCMAKE_BUILD_TYPE=$Configuration",
    "-DCMAKE_CXX_FLAGS=$genCompileFlags",
    "-DCMAKE_EXE_LINKER_FLAGS=$genLinkerFlags",
    "-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF",
    "-DSCALE_NATIVE=$(if ($Native) {'ON'} else {'OFF'})",
    "-DSCALE_WERROR=OFF",
    "-DSCALE_BUILD_TESTS=OFF"
)
if (-not $Ninja) { $genArgs += @("-A", $Arch) }

& cmake @genArgs
if ($LASTEXITCODE -ne 0) { Die "cmake configure (PGO gen) не удался" }

& cmake --build $pgoDir --config $Configuration --parallel $Jobs
if ($LASTEXITCODE -ne 0) { Die "cmake build (PGO gen) не удался" }

# --- Шаг 2: прогон на sample ------------------------------------------------
Write-Log "[PGO 2/3] Прогон на $Sample (target=$Target s)"
$sampleOut = Join-Path ([System.IO.Path]::GetTempPath()) `
                       ("pgo_" + [guid]::NewGuid().ToString("N") + ".out")

function Invoke-Train {
    $path = Get-ExePath -base $pgoDir -cfg $Configuration -name $MainExe
    if (-not $path) { Die "Не найден тренировочный бинарник $MainExe" }

    # Несколько прогонов стабилизируют профиль.
    foreach ($i in 1..3) {
        & $path -t $Target -o $sampleOut $Sample *> $null
        if ($LASTEXITCODE -ne 0) {
            Write-WarnMsg "Прогон $MainExe #$i завершился с кодом $LASTEXITCODE"
        }
    }
    # Прогон без -o — покрыть ветку «только отчёт».
    & $path $Sample *> $null

    Write-Ok "Профиль для $MainExe собран"
}

try {
    Invoke-Train
} finally {
    if (Test-Path $sampleOut) { Remove-Item -Force $sampleOut }
}

# --- Шаг 3: финальная сборка с профилем -------------------------------------
Write-Log "[PGO 3/3] Финальная сборка с профилем в $BuildDir"
$useArgs = @(
    "-S", ".", "-B", $BuildDir, "-G", $Generator,
    "-DCMAKE_BUILD_TYPE=$Configuration",
    "-DCMAKE_CXX_FLAGS=$useCompileFlags",
    "-DCMAKE_EXE_LINKER_FLAGS=$useLinkerFlags",
    "-DSCALE_PGO=ON",
    "-DSCALE_NATIVE=$(if ($Native) {'ON'} else {'OFF'})",
    "-DSCALE_WERROR=$(if ($Werror) {'ON'} else {'OFF'})",
    "-DSCALE_BUILD_TESTS=$(if ($Tests)  {'ON'} else {'OFF'})"
)
if (-not $Ninja) { $useArgs += @("-A", $Arch) }

& cmake @useArgs
if ($LASTEXITCODE -ne 0) { Die "cmake configure (PGO use) не удался" }

& cmake --build $BuildDir --config $Configuration --parallel $Jobs
if ($LASTEXITCODE -ne 0) { Die "cmake build (PGO use) не удался" }

Write-Ok "PGO-сборка завершена"

Write-Artifacts -base $BuildDir -cfg $Configuration -label "Артефакты (PGO)"

# -----------------------------------------------------------------------------
# Опциональное сравнение с baseline (BENCH=1)
# -----------------------------------------------------------------------------
if ($env:BENCH) {
    Write-Log "Сравнение времени работы (упрощённое):"
    $pgoPath  = Get-ExePath -base $BuildDir      -cfg $Configuration -name $MainExe
    $basePath = Get-ExePath -base "build-baseline" -cfg $Configuration -name $MainExe
    if ($pgoPath -and $basePath) {
        $t1 = (Measure-Command { & $basePath $Sample *> $null }).TotalMilliseconds
        $t2 = (Measure-Command { & $pgoPath  $Sample *> $null }).TotalMilliseconds
        Write-Host ("  {0,-12} baseline={1,8:N0} ms  pgo={2,8:N0} ms" -f $MainExe, $t1, $t2)
    } else {
        Write-WarnMsg "Для BENCH нужны и build-baseline/, и $BuildDir/"
    }
}

exit 0
