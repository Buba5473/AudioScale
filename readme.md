# scale

**Единая утилита для точной подгонки длительности аудиопотоков MP2, AC-3 и DTS Core под видеоряд.**  
Формат входного файла определяется автоматически. Из DTS-HD MA/HRA ядро DTS Core извлекается на лету.

[Русский](#русский) · [English](#english)

---

## Содержание

- [Русский](#русский)
  - [Зачем это нужно](#зачем-это-нужно)
  - [Как это работает](#как-это-работает)
  - [Установка](#установка)
  - [Использование](#использование)
  - [Опция `-t`](#опция--t)
  - [Поддерживаемые форматы](#поддерживаемые-форматы)
  - [Сборка из исходников](#сборка-из-исходников)
  - [Архитектура](#архитектура)
  - [Лицензия](#лицензия)
- [English](#english)
  - [Motivation](#motivation)
  - [How it works](#how-it-works-1)
  - [Installation](#installation)
  - [Usage](#usage)
  - [The `-t` option](#the--t-option)
  - [Supported formats](#supported-formats)
  - [Building from source](#building-from-source)
  - [Architecture](#architecture)
  - [License](#license-1)

---

# Русский

## Зачем это нужно

Когда записанные цифровые телепрограммы перекодируются для DVD или VCD, часто видео и аудио расходятся по времени. Если сдвиг фиксированный — справится любая программа резки MPEG. Но бывает, что рассинхронизация накапливается постепенно: один поток оказывается длиннее другого на доли секунды. Этот проект решает именно такую задачу — **немного растянуть или сжать аудиопоток**, чтобы его длина совпала с видеорядом.

## Как это работает

Аудиопотоки MP2, AC-3 и DTS Core состоят из **самодостаточных кадров** сжатых сэмплов: ни один кадр не ссылается на данные предыдущих. Поэтому:

- если поток нужно **растянуть** — утилита равномерно вставляет дублированные кадры;
- если **сжать** — равномерно удаляет отдельные кадры.

Подход грубоват, зато очень быстр, не меняет высоту тона и не требует ресемплинга. Для подгонки A/V-синхронизации этого более чем достаточно: точность — в пределах одного аудиокадра (обычно 20–30 мс).

> **Почему это не работает для MP3 и AAC?** Эти форматы используют битовый резервуар: кадр может заимствовать биты у предыдущих. Наивное дублирование или удаление кадра сломало бы поток. Поэтому MP3 и AAC не поддерживаются.

## Установка

### Готовые бинарники

Загляните в раздел [Releases](../../releases) — там лежат статические сборки `scale.exe` для Windows x64 и `scale` для Linux x86-64. Все зависимости, кроме системных, вкомпилированы в бинарник.

### Сборка из исходников

См. [Сборка из исходников](#сборка-из-исходников).

## Использование

```
scale [-t target] [-o outfile] infile
scale --help
```

| Режим | Что делает |
|---|---|
| Без `-t` и без `-o` | Только отчёт: тип аудио, каналы, размер кадра, частота, битрейт, длительность, число кадров. |
| С `-t`, без `-o` | «Сухой прогон»: сообщает, сколько кадров было бы вставлено или удалено. |
| С `-t` и `-o` | Масштабирует поток до указанной длины и записывает результат в `outfile`. |

### Примеры

```bash
# Растянуть audio.mp2 до 20.5 секунд
scale -t 20.5 -o newaudio.mp2 audio.mp2

# Ровно 500 кадров
scale -t 500f -o newaudio.ac3 audio.ac3

# До отметки 00:20:30 (hh:mm:ss)
scale -t 00:20:30 -o newaudio.dts audio.dts

# Только отчёт о свойствах входного файла
scale audio.mp2

# Сухой прогон: что было бы сделано
scale -t 20.5 audio.mp2
```

## Опция `-t`

Целевую длительность можно задать в одной из четырёх форм:

| Форма | Пример | Значение |
|---|---|---|
| Секунды (по умолчанию) | `20.5` | 20.5 секунд |
| Секунды (явно) | `20.5s` | 20.5 секунд |
| Кадры | `500f` | 500 кадров |
| Время | `01:30` | 1 мин 30 сек |
| Время | `00:20:30` | 20 мин 30 сек |

Допускается смешанная точность: `00:20:30.500`, `500.5f`. Для формы «кадры» движок использует среднюю длительность кадра по всему потоку, поэтому результат точен даже для потоков с переменным размером кадра (MP2 padding, DTS с меняющимся NBLKS).

## Поддерживаемые форматы

| Формат | Синхрослово | Особенности |
|---|---|---|
| **MP2** (MPEG-1 Layer II) | `0xFFF` (11 бит) | Размер/длительность из индексов битрейта и частоты. Padding учитывается покадрово. Layer I и III (MP3) отклоняются. |
| **AC-3** (Dolby Digital) | `0x0B77` | Размер кадра — по таблице ATSC A/52 для 48/44.1/32 кГц. Кадры фиксированные. |
| **DTS Core** | `0x7FFE8001` (BE) / `0xFE7F0180` (LE) | NBLKS × 32 сэмпла, FSIZE + 1 байт. Таблицы из ETSI TS 102 114. |
| **DTS-HD MA/HRA** | — | **Извлекается ядро** DTS Core, XLL/X96-расширение отбрасывается. На выходе — lossy 1536 кбит/с, воспроизводится любым DTS-декодером. |

Все форматы определяются автоматически: сначала DTS, затем AC-3, затем MP2. Такой порядок гарантирует, что длинное синхрослово DTS не будет ошибочно принято за мусор.

## Сборка из исходников

### Требования

- **CMake** 3.25+
- **GCC** 13+, **Clang** 17+ или **MSVC** 19.35+ (Visual Studio 2022 17.5+)
- **Ninja** (рекомендуется) или Makefiles / Visual Studio generator
- Библиотека потоков (`pthread` в Unix, встроена в Windows)

### Linux / macOS

```bash
./build.sh                       # Release в ./build
./build.sh Debug                 # Debug
./build.sh --native              # -march=native
./build.sh --werror --tests      # строгие предупреждения + тесты
./build.sh --pgo --sample sample.ac3 --target 20.5
./build.sh --clean               # удалить build-каталоги
```

### Windows (PowerShell, MSVC или clang-cl)

```powershell
.\build.ps1
.\build.ps1 -Configuration Debug -Ninja
.\build.ps1 -Pgo -Sample .\sample.dts -Target 20.5
.\build.ps1 -Tests
.\build.ps1 -Clean
```

### MSYS2 UCRT64 (статическая сборка)

В среде MSYS2 UCRT64 используйте `build_msys2.sh` — он собирает полностью статический бинарник без зависимости от `libstdc++-6.dll`, `libgcc_s_seh-1.dll` и `libwinpthread-1.dll`:

```bash
./build_msys2.sh
```

### Ручная сборка через CMake

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Опции CMake

| Опция | По умолчанию | Что делает |
|---|---|---|
| `SCALE_PGO` | `OFF` | Профильно-управляемая оптимизация |
| `SCALE_NATIVE` | `OFF` | `-march=native` вместо baseline AVX2 |
| `SCALE_WERROR` | `OFF` | Считать предупреждения ошибками |
| `SCALE_BUILD_TESTS` | `OFF` | Собрать и зарегистрировать unit-тесты |

## Архитектура

```
scale/
├── include/scale/
│   ├── common.hpp          Публичный API: Codec, Args, TargetSpec, detect_codec()
│   └── print.hpp           Shim для std::print (GCC 13 / Clang 17)
├── src/
│   ├── common.cpp          Движок: detection, поиск кадров, конвейер
│   ├── mp2codec.cpp        MP2: парсинг заголовка, padding
│   ├── ac3codec.cpp        AC-3: таблица ATSC A/52 на три частоты
│   ├── dtscodec.cpp        DTS Core + извлечение ядра из DTS-HD
│   └── main.cpp            Единственная точка входа
├── tests/                  Unit- и интеграционные тесты
├── build.sh                Кроссплатформенная сборка (Linux/macOS)
├── build.ps1               Сборка под Windows (MSVC/clang-cl)
├── build_msys2.sh          Статическая сборка в MSYS2 UCRT64
└── CMakeLists.txt
```

**Ключевые идеи:**

- **Один бинарник.** Все три формата обрабатываются одним `scale`; выбор кодека — через `Codec*`-фабрику.
- **Покадровый парсинг.** Движок вызывает `parse_header()` на каждом кадре, поэтому корректно работает с MP2 padding и DTS-HD extension.
- **`frame_span()`** — точка расширения: базовая реализация возвращает `fi.framebytes`, DTS переопределяет и сканирует следующий sync, отбрасывая XLL/X96.
- **Многопоточность.** Параллельный поиск первого кадра (`std::async` × hardware_concurrency) + конвейер reader/writer (`std::jthread` + `BoundedQueue`).
- **Bounds-safe.** Везде `std::span`, все проверки границ — до чтения. Никаких `strcpy` и `argv[++i]` без проверки.

### По сравнению с оригиналом 2004 года

- Исправлено чтение за границей буфера в цикле поиска кадров.
- Устранено беззнаковое переполнение в расчёте хвостового мусора.
- Убран `fseek(-10000)`, ломавшийся на коротких файлах.
- Исправлен разбор channel mode в MP2 (`&&` → `&`).
- Исправлены формулы размера кадра AC-3 для 44.1 и 32 кГц.
- Все `%d` заменены на `std::format`.

## Лицензия

GNU General Public License v2 или новее. Оригинальные `mp2scale` и `ac3scale` написаны Zhuo Meng в 2004 году и распространялись под той же лицензией. Поддержка DTS Core, извлечение ядра DTS-HD и переписывание на C++23 добавлены поверх этой работы; оригинальные copyright-уведомления сохранены в каждом исходном файле.

---

# English

## Motivation

When captured digital TV programs are reencoded for DVD or VCD, video and audio often drift out of sync. A fixed offset is easy to fix with any MPEG cutting tool. But sometimes the drift accumulates gradually: one stream ends up slightly longer than the other. This project solves exactly that — **slightly stretch or shrink the audio stream** so its length matches the video.

## How it works

MP2, AC-3 and DTS Core streams consist of **self-contained frames**: no frame refers back to data in previous frames. Therefore:

- to **stretch** a stream, the tool inserts evenly-spread duplicated frames;
- to **shrink** it, it drops a frame once in a while.

The approach is crude but very fast, keeps the original pitch, and requires no resampling. For A/V sync correction this is more than enough: accuracy is within one audio frame (typically 20–30 ms).

> **Why doesn't this work for MP3 and AAC?** Those formats use a bit reservoir: a frame can borrow bits from previous frames. Naively duplicating or dropping a frame would break the stream. Hence MP3 and AAC are not supported.

## Installation

### Prebuilt binaries

Check the [Releases](../../releases) section — static builds of `scale.exe` for Windows x64 and `scale` for Linux x86-64. All non-system dependencies are compiled into the binary.

### Build from source

See [Building from source](#building-from-source).

## Usage

```
scale [-t target] [-o outfile] infile
scale --help
```

| Mode | What it does |
|---|---|
| Without `-t` and `-o` | Report only: audio type, channels, frame size, sample rate, bitrate, duration, frame count. |
| With `-t`, without `-o` | Dry run: reports how many frames would be inserted or dropped. |
| With `-t` and `-o` | Scales the stream to the target length and writes it to `outfile`. |

### Examples

```bash
# Stretch audio.mp2 to 20.5 seconds
scale -t 20.5 -o newaudio.mp2 audio.mp2

# Exactly 500 frames
scale -t 500f -o newaudio.ac3 audio.ac3

# Up to 00:20:30 (hh:mm:ss)
scale -t 00:20:30 -o newaudio.dts audio.dts

# Report properties only
scale audio.mp2

# Dry run: what would be done
scale -t 20.5 audio.mp2
```

## The `-t` option

The target length can be given in one of four forms:

| Form | Example | Meaning |
|---|---|---|
| Seconds (default) | `20.5` | 20.5 seconds |
| Seconds (explicit) | `20.5s` | 20.5 seconds |
| Frames | `500f` | 500 frames |
| Time | `01:30` | 1 min 30 sec |
| Time | `00:20:30` | 20 min 30 sec |

Mixed precision is accepted: `00:20:30.500`, `500.5f`. For the frame form the engine uses the average frame duration across the whole stream, so the result is exact even for streams with variable frame size (MP2 padding, DTS with changing NBLKS).

## Supported formats

| Format | Sync word | Notes |
|---|---|---|
| **MP2** (MPEG-1 Layer II) | `0xFFF` (11 bits) | Frame size/duration from bitrate and frequency indices. Padding handled per frame. Layer I and III (MP3) are rejected. |
| **AC-3** (Dolby Digital) | `0x0B77` | Frame size from the ATSC A/52 table for 48/44.1/32 kHz. Fixed-size frames. |
| **DTS Core** | `0x7FFE8001` (BE) / `0xFE7F0180` (LE) | NBLKS × 32 samples, FSIZE + 1 bytes. Tables from ETSI TS 102 114. |
| **DTS-HD MA/HRA** | — | **DTS Core is extracted**; the XLL/X96 extension is discarded. Output is a lossy 1536 kbps stream playable by any DTS decoder. |

All formats are auto-detected: DTS first, then AC-3, then MP2. This order ensures the long DTS sync word is never mistaken for junk.

## Building from source

### Requirements

- **CMake** 3.25+
- **GCC** 13+, **Clang** 17+ or **MSVC** 19.35+ (Visual Studio 2022 17.5+)
- **Ninja** (recommended) or Makefiles / Visual Studio generator
- Threads library (`pthread` on Unix, built-in on Windows)

### Linux / macOS

```bash
./build.sh                       # Release into ./build
./build.sh Debug                 # Debug
./build.sh --native              # -march=native
./build.sh --werror --tests      # strict warnings + tests
./build.sh --pgo --sample sample.ac3 --target 20.5
./build.sh --clean               # remove build directories
```

### Windows (PowerShell, MSVC or clang-cl)

```powershell
.\build.ps1
.\build.ps1 -Configuration Debug -Ninja
.\build.ps1 -Pgo -Sample .\sample.dts -Target 20.5
.\build.ps1 -Tests
.\build.ps1 -Clean
```

### MSYS2 UCRT64 (static build)

In the MSYS2 UCRT64 environment use `build_msys2.sh` — it produces a fully static binary with no dependency on `libstdc++-6.dll`, `libgcc_s_seh-1.dll` or `libwinpthread-1.dll`:

```bash
./build_msys2.sh
```

### Manual CMake build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### CMake options

| Option | Default | What it does |
|---|---|---|
| `SCALE_PGO` | `OFF` | Profile-guided optimization |
| `SCALE_NATIVE` | `OFF` | `-march=native` instead of AVX2 baseline |
| `SCALE_WERROR` | `OFF` | Treat warnings as errors |
| `SCALE_BUILD_TESTS` | `OFF` | Build and register unit tests |

## Architecture

```
scale/
├── include/scale/
│   ├── common.hpp          Public API: Codec, Args, TargetSpec, detect_codec()
│   └── print.hpp           std::print shim (GCC 13 / Clang 17)
├── src/
│   ├── common.cpp          Engine: detection, frame scanning, pipeline
│   ├── mp2codec.cpp        MP2: header parsing, padding
│   ├── ac3codec.cpp        AC-3: ATSC A/52 table for three sample rates
│   ├── dtscodec.cpp        DTS Core + DTS-HD core extraction
│   └── main.cpp            Single entry point
├── tests/                  Unit and integration tests
├── build.sh                Cross-platform build (Linux/macOS)
├── build.ps1               Windows build (MSVC/clang-cl)
├── build_msys2.sh          Static build in MSYS2 UCRT64
└── CMakeLists.txt
```

**Key ideas:**

- **One binary.** All three formats are handled by a single `scale`; codec selection goes through a `Codec*` factory.
- **Per-frame parsing.** The engine calls `parse_header()` on every frame, so MP2 padding and DTS-HD extension are handled correctly.
- **`frame_span()`** is the extension point: the base implementation returns `fi.framebytes`; DTS overrides it and scans for the next sync, discarding the XLL/X96 extension.
- **Concurrency.** Parallel first-frame search (`std::async` × hardware_concurrency) plus a reader/writer pipeline (`std::jthread` + `BoundedQueue`).
- **Bounds-safe.** Everything goes through `std::span`; all size checks happen before access. No `strcpy`, no unchecked `argv[++i]`.

### Compared to the original 2004 version

- Fixed out-of-bounds read in the frame-search loop.
- Removed unsigned underflow in the trailing-junk calculation.
- Removed `fseek(-10000)`, which broke on short files.
- Fixed MP2 channel-mode parse (`&&` → `&`).
- Fixed AC-3 frame-size formulas for 44.1 and 32 kHz.
- Replaced all `%d` with `std::format`.

## License

GNU General Public License v2 or later. The original `mp2scale` and `ac3scale` were written by Zhuo Meng in 2004 and distributed under the same license. DTS Core support, DTS-HD core extraction, and the C++23 rewrite were added on top of that work; the original copyright notices are preserved in every source file.
