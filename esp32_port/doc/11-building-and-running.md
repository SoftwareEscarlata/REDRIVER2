# Building, flashing and playing

This is the operational document for the ESP32-S3 port of Driver 2. It covers what
you need installed, how the firmware is actually built, how the game data image is
produced, exactly what goes where in flash, and how to drive the car once it boots.

Everything here is taken from the scripts and sources in `esp32_port/`. Where a
command is not wrapped in a script, the raw command line is given, because there is
no `flash.ps1` in the tree despite `build.ps1` advertising one (see
[Flashing](#flashing) below).

---

## 1. Prerequisites

### Hardware

| Item | Value |
|---|---|
| Board | Waveshare ESP32-S3-Touch-LCD-2 |
| SoC | ESP32-S3, dual Xtensa LX7 @ 240 MHz |
| Internal SRAM | 512 KB (only ~20 KB free once the app is up) |
| PSRAM | 8 MB octal, 80 MHz |
| Flash | 16 MB |
| Panel | ST7789, 320x240 landscape, SPI @ 80 MHz |
| Host connection | the board's **native USB** port (ESP32-S3 USB-Serial-JTAG) |

The port targets this board specifically. `esp32_port/main/board_pins.h` documents the
pin map and states it was verified against the vendor schematic
(`ESP32-S3-Touch-LCD-2-SchDoc.pdf`) and the vendor ESP-IDF demos, not guessed.

One board-level constraint matters before you plug anything in: header **P1 doubles as
the DVP camera bus** (IO2/4/6/7/8/10). `board_pins.h:48-49` says it plainly — *"Camera
must stay unplugged"*. If a camera module is attached, the game buttons will not work.

### Toolchain

These are absolute paths hard-coded in `esp32_port/build.ps1:10-12`. They are not
discovered, so if your install differs you edit those three lines:

```powershell
$IDF_PATH  = "C:\Espressif5.5\frameworks\esp-idf-v5.5.4"
$TOOLS     = "C:\Espressif5.5"
$PY_VENV   = "C:\Espressif\python_env\idf4.4_py3.10_env"
```

| Component | Path | Notes |
|---|---|---|
| ESP-IDF | `C:\Espressif5.5\frameworks\esp-idf-v5.5.4` | v5.5.4 |
| Tools root | `C:\Espressif5.5` | xtensa-esp-elf, cmake, ninja, ccache |
| Python venv | `C:\Espressif\python_env\idf4.4_py3.10_env` | see below |
| esptool | `$IDF_PATH\components\esptool_py\esptool\esptool.py` | reports `v4.12.dev3` |

The Python venv is the odd one. It is named for IDF 4.4 but is used to drive IDF 5.5.4.
`build.ps1:2-4` records why: this is the exact environment velxio's `espidf_compiler`
already proved working on this machine, and *"the 4.4-era python venv ... has the v5.x
requirements installed"*. It is a known-good combination, not an accident, and the same
recipe as the OpenLara and Descent ports on the same board.

### Sources

Two repositories, both on branch `esp32s3`:

```
SoftwareEscarlata/REDRIVER2                                  ->  E:/Hardware/Driver2/REDRIVER2
SoftwareEscarlata/PsyCross  (submodule, remote named "fork") ->  .../src_rebuild/PsyCross
```

`.gitmodules` still points the submodule at upstream `OpenDriver2/PsyCross`, so after a
fresh clone the submodule will land on upstream. Inside `src_rebuild/PsyCross`, `origin`
is upstream and `fork` is the port; you want `fork/esp32s3` checked out, which is where
the software rasterizer lives.

---

## 2. Building

### Why not `idf.py`

`build.ps1` sets the environment up by hand and calls `cmake -G Ninja` and `ninja`
directly. `idf.py` is a wrapper that does approximately this, plus environment discovery
that fails on this machine's split installation: an IDF 5.5.4 framework tree under
`C:\Espressif5.5` paired with a Python virtualenv under `C:\Espressif`. `idf.py` would
try to reconcile those and re-provision tools.

Driving CMake directly removes all of that. It also lets the script apply the one
workaround IDF 5.x needs here, `build.ps1:19-20`:

```powershell
# IDF 5.x idf_tools.py fatals if MSYSTEM is present
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
```

`MSYSTEM` is set by Git Bash / MSYS2 shells. If it leaks into the environment,
`idf_tools.py` decides it is running under MSYS and aborts. Because the build is
frequently launched from a shell that has it, the script strips it unconditionally.

### The build command

```powershell
powershell -ExecutionPolicy Bypass -File E:\Hardware\Driver2\REDRIVER2\esp32_port\build.ps1
powershell -ExecutionPolicy Bypass -File E:\Hardware\Driver2\REDRIVER2\esp32_port\build.ps1 clean
```

What it does, in order (`build.ps1:14-51`):

1. Exports `IDF_PATH`, `IDF_TOOLS_PATH`, `IDF_TARGET=esp32s3`, `IDF_PYTHON_ENV_PATH`
   and `VIRTUAL_ENV`.
2. Removes `MSYSTEM`.
3. Prepends to `PATH`, in this order: the venv `Scripts`, the xtensa toolchain `bin`,
   cmake `bin`, ninja, and ccache. Each is resolved with a wildcard `Get-ChildItem` and
   `Select-Object -First 1`, so a toolchain version bump does not require editing the
   script.
4. `cmake -G Ninja -Wno-dev -DIDF_TARGET=esp32s3 -DCMAKE_BUILD_TYPE=Release
   -DSDKCONFIG_DEFAULTS=<proj>\sdkconfig.defaults -DCCACHE_ENABLE=1 -S <proj> -B <proj>\build`
5. `ninja -C <proj>\build`

Output is `esp32_port/build/driver2_esp32s3.bin` (674 KB at the time of writing).

`ccache` is enabled deliberately: the component compiles the whole of Driver 2 plus
PsyCross — several hundred translation units — and a cold build is slow enough that
cache hits on unchanged game code dominate iteration time.

### What gets compiled

`esp32_port/main/CMakeLists.txt` globs the game and the host-independent half of
PsyCross, and drops the pieces that assume a desktop:

| Excluded | Reason (from the CMakeLists comments) |
|---|---|
| `LIBCD` (`SRC_PSX` filter, line 25) | emulates a CD drive with an SDL spooler thread; game data is memory-mapped flash here, so the CD API is stubbed in `esp_stubs.cpp` |
| `utils/fs.cpp` (line 32) | needs POSIX `glob()` for directory enumeration; the flash VFS has no directories |
| `video_source`, `audio_source`, `VideoPlayer` (line 32) | FMV playback, no backend on this target |
| `xaplay` (line 33) | XA audio |
| `asmtest` (line 20) | test harness |

Two inclusions are easy to get wrong and are called out in the file:
`Game/ASM/*.c` holds the **portable C versions** of the PSX assembly routines (renderer
inner loops, RNC decompression, map lookups) and `Game/*.c` holds the locale tables —
*"Both are required"* (lines 14-17).

As upstream does, the game's `.c` files are compiled as C++ (`set_source_files_properties
... LANGUAGE CXX`, line 43-45), guarded by `if(NOT CMAKE_BUILD_EARLY_EXPANSION)` because
that command is illegal during IDF's requirement-scan pass.

### Compile definitions that matter

`main/CMakeLists.txt:69-74`:

```cmake
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    NTSC_VERSION RELEASE ESP32_PORT
    USE_PGXP=0 USE_EXTENDED_PRIM_POINTERS=1
    PSYX_NO_RENDERER=1
    SOFTRAS_PROFILE=1
)
```

| Define | Effect |
|---|---|
| `NTSC_VERSION` | Selects `NTSCScreenNames` in `main.c:1846`, i.e. the boot splash is `GFX\SPLASH1N.TIM`. This is why that file must be in the data image (§3). |
| `ESP32_PORT` | Guards the target-specific paths. Counted over `src_rebuild/Game` and `src_rebuild/PsyCross`: **seven files test it, at 15 sites**. Game tree (3 files, 4 sites): `Game/C/system.c` (the 2 MB PSX RAM arena becomes `EXT_RAM_BSS_ATTR`, i.e. PSRAM `.bss`, and `free()` is skipped for the mmap'd level data), `Game/C/spool.c` (the level spooler uses the flash pointer in place instead of `malloc(16MB)`), `Game/driver2.h` (`trap()` stops emitting x86 `int3`). PsyCross (4 files, 11 sites): `PsyX_render.h` and `PsyX_GPU.cpp` shrink the unused GL vertex/split buffers from 1.9 MB to a few KB and move them to PSRAM (`MAX_VERTEX_BUFFER_SIZE` drops to 64 in `PsyX_render.h:122`, `MAX_DRAW_SPLITS` to 4 in `PsyX_GPU.cpp:60` — note it is defined in the `.cpp`, not in `PsyX_GPU.h` — and `g_vertexBuffer`/`g_splits` gain `EXT_RAM_BSS_ATTR`); the rest is the dual-core split — `PsyX_GPU.cpp` again (fork/join around the OT walk and the two rendezvous primitives), `PsyX_GPU.h` (`activeDrawEnv` becomes `g_drawEnv[xPortGetCoreID()]`, plus the fork/join declarations) and `PsyX_SoftRas.cpp` (the primitive render state becomes `__thread`, and the band slot is the core id). |
| `USE_PGXP=0` | Keeps `VERTTYPE` a plain integer `short`. The PGXP path uses a software half-float type that is unusable here. |
| `USE_EXTENDED_PRIM_POINTERS=1` | Kept at 1 — with `USE_PGXP=0` this is the configuration the upstream Win32 CI already exercises. |
| `PSYX_NO_RENDERER=1` | Builds the whole `GR_*` API surface without an OpenGL backend, so the software rasterizer owns the frame. |
| `SOFTRAS_PROFILE=1` | Enables the per-frame profiling counters and the second serial log line (§6). Costs a pair of `esp_cpu_get_cycle_count()` reads per band and per present. Drop it for a clean run. |

Two more things are applied per-source rather than globally (lines 76-83):

```cmake
COMPILE_OPTIONS "-w;-fpermissive;-Wno-narrowing;-include<...>/esp_compat.h"
```

`-w -fpermissive -Wno-narrowing` matches the permissiveness the upstream Linux build uses
for C-compiled-as-C++. The force-included `esp_compat.h` is load-bearing: it is what
redirects every `fopen` in the game to the flash VFS, and it defines `D2_PSRAM`:

```c
#define fopen(name, mode) ((FILE*)esp_game_fopen((name), (mode)))
...
#define D2_PSRAM EXT_RAM_BSS_ATTR
```

### Key `sdkconfig.defaults` settings

| Setting | Why |
|---|---|
| `CONFIG_ESPTOOLPY_FLASHSIZE_16MB` | The partition table runs to the end of a 16 MB part. |
| `CONFIG_SPIRAM_MODE_OCT` + `CONFIG_SPIRAM_SPEED_80M` | Octal PSRAM at 80 MHz. The comment lists the tenants: *"PSX RAM arena (2MB) + emulated VRAM (1MB) + PsyX statics (~2.7MB) live here"*. |
| `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` | Required for `EXT_RAM_BSS_ATTR` / `D2_PSRAM` to work at all. |
| `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY` | The game task's 96 KB stack is allocated from PSRAM in `esp_main.cpp:116`. |
| `CONFIG_ESP32S3_DATA_CACHE_64KB` + `_LINE_64B` | *"required-safe with octal PSRAM and is what the texture-heavy rasterizer wants"*. |
| `CONFIG_COMPILER_OPTIMIZATION_PERF` | `-O2`. This is a rasterizer; it matters. |
| `CONFIG_ESP_TASK_WDT_INIT=n` | The task watchdog is off. A frame can take 60-200 ms and the game task never yields inside it. |
| `CONFIG_PARTITION_TABLE_CUSTOM` + `..._FILENAME="partitions.csv"` | §4. |

---

## 3. Game data

### Getting the demo set

Driver 2 game data is not redistributable, but the OpenDriver2 project publishes a free
demo set for its own browser build. `esp32_port/get_demo_data.py` fetches exactly that:

```powershell
python E:\Hardware\Driver2\REDRIVER2\esp32_port\get_demo_data.py
```

It downloads `REDRIVER2.js` and `REDRIVER2.data` (~30 MB) from
`raw.githubusercontent.com/OpenDriver2/opendriver2.github.io/master/webgame/`, parses the
emscripten `loadPackage({...})` manifest out of the JS, and slices the `DRIVER2DEMO/`
files plus `demo_config.ini` out of the blob by byte range. Default destination is
`REDRIVER2/data/`, producing `REDRIVER2/data/DRIVER2DEMO/`.

That yields 31 files, 15.56 MB (16,315,524 bytes): the demo Havana level, frontend
assets, language tables, one mission (`M78.D2MS`), and the sound banks.

```
DATA/     CARCONT.RAW CREDITS.ENG FEFONT.BNK GFX.RAW SCRS.BIN SKY1.RAW    1.07 MB
GFX/      FELOAD.TIM FONT2.FNT LOADHAVA.TIM SPLASH1N.TIM + HQ/            5.24 MB
LANG/     EN/FR/GE/IT/SP _GAME.LTXT and _MISSION.LTXT                       15 KB
LEVELS/   HAVANA.DEN HAVANA.LCF HAVANA.LEV                                5.99 MB
MISSIONS/ M78.D2MS                                                           5 KB
SOUND/    MUSIC.BIN VOICES2.BLK                                           3.24 MB
```

(`demo_config.ini` is only needed for the PC build, where it sets
`dataFolder=DRIVER2DEMO`. The ESP32 build sets `gDataFolder[0] = 0` in
`esp_main.cpp:87` because the container already stores paths relative to the data root.)

### Building the OLVL image

```powershell
cd E:\Hardware\Driver2\REDRIVER2\esp32_port
python make_data.py build\gamedata.bin
```

Result: **26 files, 10.95 MB** (11,487,176 bytes).

The container format is shared with the OpenLara and Descent ports
(`make_data.py:3-7`):

```
u32 magic 'OLVL' | u32 count | count * { char name[32]; u32 off; u32 size }
then 4-byte-aligned file blobs
```

The 4-byte alignment is not cosmetic — it is what makes the mmap'd pointers usable in
place, which is what lets `spool.c` read the 6 MB level file straight out of flash with
zero RAM. Names keep their relative path with `/` separators; `esp_fs.c:63-74` matches
case-insensitively and treats `\` and `/` as the same character, so the game's DOS-style
`"DATA\\GFX.RAW"` resolves without touching the game code.

Name length is capped at 31 characters and the script hard-errors rather than silently
truncating (`make_data.py:43-44`).

### What must NOT be excluded

`make_data.py` has three exclusion lists. Only the first is populated:

```python
SKIP_DIRS = {"GFX/HQ"}
# note: the splash TIMs must stay — the boot sequence blocks without them
SKIP_FILES = set()
SOUND_FILES = {"SOUND/VOICES2.BLK", "SOUND/MUSIC.BIN"}
```

**`GFX/HQ` (4.61 MB) is safe to drop.** It is PC-only high-resolution font TGAs
(`digits.tga`, `fefont.tga`, `font2.tga` and their `.fn2` metrics) used by the desktop
renderer's hi-res text path. Nothing in the software rasterizer path reads them.

**`GFX/SPLASH1N.TIM` must ship.** `SKIP_FILES` is empty and the comment above it says why.
The chain is: `NTSC_VERSION` selects `NTSCScreenNames` (`main.c:1765-1770`), whose only
entry is `"GFX\\SPLASH1N.TIM"`; `main.c:1848` has a fallback, but it only swaps in the
*PAL* name `SPLASH1P.TIM`, which the demo set does not contain either. So with
`SPLASH1N.TIM` excluded there is no working name, and `ShowHiresScreens(ScreenNames, 300, 0)`
at `main.c:1885` — the first thing that runs after language init — does not complete.
Commit `fa95b9a6` records the observed symptom: a black screen, never reaching the
frontend.

**The sound banks must ship, even though there is no audio yet.** This is stated twice,
once in `make_data.py` and once in `partitions.csv:6-7`:

> The sound banks must ship even before audio exists — the frontend blocks on a failed
> `VOICES2.BLK` open.

`LoadBankFromLump` (`gamesnd.c:144-170`) reads a 73-entry block table out of
`SOUND\VOICES2.BLK` before it can size any lump, and `InitMusic` (`gamesnd.c:1758-1781`)
does the same with `SOUND\MUSIC.BIN` to find the sample and music offsets. With the files
absent, `LoadfileSeg` returns 0 and those offsets stay zero.

The `--no-sound` flag still exists and still produces a valid image (24 files, 7.71 MB),
but it does not boot to gameplay. It is kept for size experiments only. Commit `fa95b9a6` describes
this as the first of the boot failures, in the order they appeared:

> `GFX/SPLASH1N.TIM` and the `SOUND` banks were excluded from the data image to save
> flash. The boot sequence blocks on both, so ship them; the gamedata partition grows to
> the end of the 16MB flash.

That is the reason the partition is 11.94 MB rather than the 11 MB in the original plan.

---

## 4. Partition layout

`esp32_port/partitions.csv`:

```
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xf000,   0x1000,
factory,  app,  factory, 0x10000,  0x400000,
# demo data (OLVL container): Havana level + frontend assets + sound banks.
# The sound banks must ship even before audio exists — the frontend blocks on
# a failed VOICES2.BLK open. Sized to the end of the 16MB flash.
gamedata, data, 0x40,    0x410000, 0xBF0000,
```

| Region | Offset | Size | Contents | Actual use |
|---|---|---|---|---|
| bootloader | `0x0` | — | `build/bootloader/bootloader.bin` | |
| partition table | `0x8000` | 4 KB | `build/partition_table/partition-table.bin` | `CONFIG_PARTITION_TABLE_OFFSET 0x8000` |
| `nvs` | `0x9000` | 24 KB | unused today | |
| `phy_init` | `0xf000` | 4 KB | unused (no radio) | |
| `factory` | `0x10000` | 4 MB | `build/driver2_esp32s3.bin` | 674 KB — 16 % |
| `gamedata` | `0x410000` | 11.94 MB | `build/gamedata.bin` | 10.95 MB — 92 % |

`0x410000 + 0xBF0000 = 0x1000000`, exactly the end of the 16 MB part.

`gamedata` uses a custom data subtype `0x40` — an arbitrary application-defined value, not
one of IDF's standard subtypes, because it is neither FAT nor SPIFFS nor NVS. The
firmware looks it up by name and subtype in `esp_fs.c:222-224`:

```c
const esp_partition_t* part = esp_partition_find_first(
    ESP_PARTITION_TYPE_DATA, 0x40, "gamedata");
```

It is then mapped read-only with `esp_partition_mmap(..., ESP_PARTITION_MMAP_DATA, ...)`
and the magic is checked before anything else touches it.

---

## 5. Flashing

`build.ps1:55` prints `Flash app : powershell -File flash.ps1 [COMx]`. **There is no
`flash.ps1` in the tree.** Flashing is done with `esptool.py` directly. The exact
arguments IDF itself would use are in `build/flash_args`:

```
--flash_mode dio --flash_freq 80m --flash_size 16MB
0x0 bootloader/bootloader.bin
0x10000 driver2_esp32s3.bin
0x8000 partition_table/partition-table.bin
```

Set up a shell once:

```powershell
$PY      = "C:\Espressif\python_env\idf4.4_py3.10_env\Scripts\python.exe"
$ESPTOOL = "C:\Espressif5.5\frameworks\esp-idf-v5.5.4\components\esptool_py\esptool\esptool.py"
$PORT    = "COM6"
cd E:\Hardware\Driver2\REDRIVER2\esp32_port\build
```

### Application only (the common case)

674 KB, a couple of seconds. This is what you run after every code change.

```powershell
& $PY $ESPTOOL --chip esp32s3 -p $PORT -b 921600 `
    --before=default_reset --after=hard_reset `
    write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB `
    0x10000 driver2_esp32s3.bin
```

### Bootloader + partition table

Needed on a virgin board, and again whenever `partitions.csv` changes.

```powershell
& $PY $ESPTOOL --chip esp32s3 -p $PORT -b 921600 `
    --before=default_reset --after=hard_reset `
    write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB `
    0x0 bootloader/bootloader.bin `
    0x8000 partition_table/partition-table.bin
```

### Game data image

10.95 MB, roughly a minute at 921600 baud. Only needed when the data set changes — which
is why it is deliberately *not* part of `flash_args`. Keeping the 10.95 MB image out of
the normal flash cycle is the difference between a two-second and a one-minute edit loop.

```powershell
& $PY $ESPTOOL --chip esp32s3 -p $PORT -b 921600 `
    --before=default_reset --after=hard_reset `
    write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB `
    0x410000 gamedata.bin
```

### All three at once

```powershell
& $PY $ESPTOOL --chip esp32s3 -p $PORT -b 921600 `
    --before=default_reset --after=hard_reset `
    write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB `
    0x0 bootloader/bootloader.bin `
    0x8000 partition_table/partition-table.bin `
    0x10000 driver2_esp32s3.bin `
    0x410000 gamedata.bin
```

> **Note on `--flash_mode dio`.** `sdkconfig.defaults` asks for
> `CONFIG_ESPTOOLPY_FLASHMODE_QIO=y`, but the generated config resolves
> `CONFIG_ESPTOOLPY_FLASHMODE` to `"dio"` and `flash_args` therefore says `dio`. Use what
> `flash_args` says; the image header written by `elf2image` and the flashing mode must
> agree, and the second-stage bootloader reconfigures the flash from the header at
> runtime.

---

## 6. Watching the serial log

The console is the ESP32-S3's **built-in USB-Serial-JTAG** peripheral
(`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y`, `CONFIG_ESP_CONSOLE_UART_BAUDRATE=115200`).
There is no external USB-UART bridge chip. That single fact explains most of §8.

IDF's monitor, which symbolises panic backtraces using the ELF:

```powershell
& $PY -m esp_idf_monitor --toolchain-prefix xtensa-esp32s3-elf- `
    --target esp32s3 -p COM6 `
    E:\Hardware\Driver2\REDRIVER2\esp32_port\build\driver2_esp32s3.elf
```

Any plain terminal at 115200 also works, and `drive.ps1` echoes the log itself (§7), so in
practice you use the monitor for crashes and `drive.ps1` for everything else.

### A healthy boot

```
I (xxx) driver2: Driver 2 ESP32-S3 (REDRIVER2 + software rasterizer)
I (xxx) driver2: PSRAM: 8192 KB
I (xxx) esp_fs: gamedata: 26 files mounted at 0x3c...
W (xxx) psx_plat: button gpio17 stuck low at boot - disabled
I (xxx) driver2: internal free: NN KB (largest NN KB), PSRAM free: NNNN KB
```

The `gpio17` warning is expected on this board and is not a fault — see §7.

Then, once per 60 frames, from `PsyX_EndScene` (`esp_host.cpp:82-118`):

```
I (xxx) psx_host: 16.8 fps  320x240  2400 prims/frame
I (xxx) psx_host:   raster c0 29 + c1 32 ms/f  present 0  other 27 | ... tris/f  ... kpx/f  68 cyc/px  diverge 0
I (xxx) psx_host:   worker stack free 4376 B
```

`tris/f` and `kpx/f` are left as ellipses on purpose: they are raw scene counters
that swing by a factor of two or more between standing still and driving, so no
single value belongs in a sample transcript. Everything else on the line is a
representative in-game figure.

Reading that line:

| Field | Meaning |
|---|---|
| `320x240` | the display env the game is currently asking for. The frontend runs `640x512` and the game `320x240`; the present path rescales either onto the panel. |
| `raster c0 / c1` | per-core band time. The frame waits for both, so the **larger** number is the critical path. Measured 29 vs 32 ms at 16.8 fps. |
| `present` | 0, because the panel flush was moved to a core-1 task. It was 17 ms when inline, of which almost all was DMA wait (320x240x2 bytes at 80 MHz = 15.4 ms). |
| `other` | frame time minus the slower band: game logic plus the software GTE. 27 ms. |
| `cyc/px` | cycles per covered pixel. **68** today, down from 221 before the rasterizer work. |
| `diverge` | must be **0**. Both cores replay the same packet stream into their own `g_drawEnv[core]` slot and must end bit-identical; a non-zero value means the draw environments drifted apart. |
| `worker stack free` | the core-1 rasterizer worker is created with a **6144-byte** internal-RAM stack (`esp_raster.cpp:68`) and the high-water **free** figure is **4376 bytes**, i.e. about 1.7 KB deepest observed use. It has to be internal RAM because an interrupt runs on the interrupted task's stack. |

### Failure lines

| Line | Meaning |
|---|---|
| `E ... esp_fs: gamedata partition missing` | The partition table on the device does not contain `gamedata`. Reflash the partition table. |
| `E ... esp_fs: bad OLVL magic XXXXXXXX — flash game data first` | The partition exists but does not start with `'OLVL'`. See §8. |
| `E ... driver2: game data mount failed — flash gamedata.bin first` | Follow-on from either of the above; `app_main` returns and nothing runs. |
| `E ... driver2: PSRAM alloc failed: <name> (N KB)` | One of the PSX buffers in `allocPsxMemory()` did not fit. |

---

## 7. Playing

### Physical buttons on header P1

All buttons are **active low to the common GND at P1 pin 13**. `board_pins.h:45-62`:

| P1 pin | GPIO | PSX button | Notes |
|---|---|---|---|
| 1 | IO2 | D-pad Up | internal pull-up |
| 2 | IO4 | D-pad Down | internal pull-up |
| 3 | IO6 | D-pad Left | internal pull-up |
| 4 | IO16 | D-pad Right | 4.7K hardware pull-up on board (camera SCCB) |
| 5 | IO17 | Cross | **unusable — see below** |
| 6 | IO18 | Square | internal pull-up |
| 7 | IO21 | L1 | 4.7K hardware pull-up on board (camera SCCB) |
| 8 | IO8 | R1 | internal pull-up |
| 9 | IO7 | Start | internal pull-up |
| 10 | IO10 | Select | internal pull-up |
| — | GPIO0 (BOOT) | Cross | the on-board BOOT button, kept as a second Cross |
| 13 | GND | common | |
| 14 | 5V | | |

Wiring is a momentary switch from each GPIO to pin 13. Internal pull-ups are enabled for
every line in `esp_input_init()`, so no external resistors are needed.

**IO17 does not work on this board.** `board_pins.h:54-56`:

> `IO17` measures 0 at reset even with the internal pull-up enabled, so this board drives
> it. `esp_input_init()` detects that and disables the line; CROSS is still reachable on
> GPIO0 (BOOT) and over serial.

This was not a wiring mistake — it was a real bug that cost a debugging session. Commit
`fa95b9a6`: *"GPIO17 is held low by this board, so the A button read as permanently
pressed and the menu never saw a new press."* A permanently-held button is invisible to
edge-triggered menu code, so the frontend simply stopped responding. The fix is generic
rather than a hard-coded exclusion of pin 17 (`esp_platform.cpp:239-245`):

```c
// A line that is already low at reset is not a pressed button — it is a pin
// this board drives itself ...
for (unsigned i = 0; i < sizeof(sButtons) / sizeof(sButtons[0]); i++) {
    const int level = gpio_get_level((gpio_num_t)sButtons[i].gpio);
    if (!level) {
        sButtonStuck |= 1u << i;
        ESP_LOGW(TAG, "button gpio%d stuck low at boot - disabled", sButtons[i].gpio);
    }
}
```

The consequence for the player: **hold no buttons while the board resets**, or that button
will be disabled for the whole session.

### Driving from the PC keyboard: `drive.ps1`

```powershell
cd E:\Hardware\Driver2\REDRIVER2\esp32_port
.\drive.ps1                 # defaults to COM6
.\drive.ps1 -Port COM7
.\drive.ps1 -Quiet          # do not echo the board's log
```

| Key | PSX button | In game |
|---|---|---|
| Left / Right arrow, A / D | D-pad Left / Right | steer |
| Up / Down arrow, W / S | D-pad Up / Down | menu navigation |
| X | Cross | accelerate; confirm in menus |
| Z | Square | brake / reverse |
| Space | Triangle | handbrake |
| C | Circle | wheelspin |
| Shift | L1 | fast steer |
| Ctrl | R1 | |
| Enter | Start | |
| Backspace | Select | |
| P / V / T / R | — | debug dumps: screen / whole VRAM / decoded texture page / raw VRAM words |
| Esc | — | quit, releasing the pad |

Keys are only read while the `drive.ps1` console window has focus
(`Kbd::Focused()` compares `GetForegroundWindow()` with `GetConsoleWindow()`); when it
loses focus the mask is forced to zero so alt-tabbing away does not leave the throttle on.

### The pad-state protocol

This exists because of a limitation that is easy to miss. `drive.ps1:3-5`:

> A serial console only transmits key PRESSES, so it can tap a button but never hold and
> release one — no good for a throttle or a steering wheel.

A terminal gives you a character on key-down and nothing on key-up. That is fine for menus
and useless for driving. So the host sends the **complete pad word** instead:

```
"=" HHHH
```

Four hex digits, **active high**, sent as often as the host likes.
`drive.ps1:102` sends it unconditionally every 20 ms (50 Hz), even when unchanged.

Bit assignment (`drive.ps1:32-37`, matching `esp_platform.cpp:174-187` and the word the
game builds as `(buttons[0] << 8) | buttons[1]` in `pad.c`):

| Bit | Button | Bit | Button |
|---|---|---|---|
| `0x0001` | L2 | `0x0080` | Square |
| `0x0002` | R2 | `0x0100` | Select |
| `0x0004` | L1 | `0x0800` | Start |
| `0x0008` | R1 | `0x1000` | Up |
| `0x0010` | Triangle | `0x2000` | Right |
| `0x0020` | Circle | `0x4000` | Down |
| `0x0040` | Cross | `0x8000` | Left |

Three properties make it robust, all deliberate (`esp_platform.cpp:254-262`):

- **Stateless.** Each packet is the complete state, so a dropped or corrupted byte
  self-corrects on the next update 20 ms later. The parser consumes exactly four hex
  digits after `=` and resyncs on any non-hex character.
- **It times out.** `HOST_MASK_TIMEOUT_MS 400`. If the host stops sending, everything
  releases. Closing the window mid-corner cannot leave the throttle stuck on.
- **It coexists with the old taps.** `=` cannot collide with a single-character command,
  so `w/a/s/d`, `x/z/q/e`, Enter, Space and `1`/`2` still work from a plain terminal
  (`serialMask`, `esp_platform.cpp:203-220`). Those set a `SERIAL_HOLD` of 6 frames, which
  is why they can tap but not hold.

`drive.ps1` writes `=0000` in its `finally` block before closing the port, so a clean exit
releases immediately rather than waiting out the 400 ms.

One implementation detail this protocol forced: `esp_input_poll` originally drained a
fixed 16 bytes per frame. A 5-byte packet at 50 Hz outruns any per-frame cap, the receive
buffer backs up, and the host's `Write` eventually blocks and times out. It now drains the
buffer completely (`esp_platform.cpp:283-321`).

### Other serial commands

Single characters the firmware acts on, sent from any terminal:

| Char | Action |
|---|---|
| `p` | dump the visible display area as 160x120 RGB332 hex between `#DUMP`/`#ENDDUMP` — reconstructs the panel image on the host without a camera |
| `v` | dump the whole 1024x512 VRAM at half scale |
| `t` | dump one texture page, decoded exactly as the rasterizer samples it |
| `r` | dump raw VRAM words of one texture page between `#RAW`/`#ENDRAW` |
| `f` | toggle the texture minification filter live |

`f` is worth calling out because **`drive.ps1` does not bind it** — its `$DUMP` table only
covers `p`, `v`, `t`, `r`. To flip the filter you send `f` from a plain terminal, or add
`0x46 = 'f'` to `$DUMP` in `drive.ps1:60`. It is a live A/B so the two can be compared on
the panel without reflashing, and the cost is real: toggling it mid-scene measured
**14.5 → 10.0 fps**, roughly a third of the frame rate. Note that 14.5 fps is the
baseline of that particular A/B scene — it was exercised in a heavier part of the level —
and not the port's headline unfiltered rate of 16.8 fps; the pair is only meaningful
because both halves are the same scene seconds apart. While actually driving with the
filter on, the measured rate is 5-9 fps.

---

## 8. Troubleshooting

These are the failures actually hit during the port, not a generic list.

### The board disappears from USB after a crash loop

**Symptom.** The COM port vanishes, or reappears and vanishes every second or so.
`esptool` fails to connect. The monitor prints nothing or dies immediately.

**Cause.** The console is the ESP32-S3's *internal* USB-Serial-JTAG peripheral, not an
external bridge chip. There is no independent USB device: when the application panics and
the chip reboots, the USB device goes away with it and re-enumerates on the next boot. In
a panic loop that happens continuously, and Windows cannot keep a stable COM port
assigned to a device that keeps disconnecting. This is different from boards with a CP210x
or CH340, where the bridge stays enumerated no matter what the SoC does.

**Fix.** Unplug and replug the USB cable. If the loop restarts immediately, hold the
**BOOT** button (GPIO0) while plugging in: the ROM download stub holds the USB device up
without running the application, so the port stays put and you can flash a fixed image.

### Only one program can hold the COM port

**Symptom.** `esptool` reports the port is in use; or `drive.ps1` prints
`cannot open COM6`; or the monitor shows nothing while `drive.ps1` is running.

**Cause.** Windows serial ports are opened exclusively. `esptool.py`, `esp_idf_monitor`
and `drive.ps1` all open the same `COM6`, and the first one wins.

**Fix.** Close the other one. In practice: run `drive.ps1` (which echoes the board's log
anyway, so you rarely need a separate monitor), close it before flashing, reopen it after.
`drive.ps1` releases the port in its `finally` block, so Esc is enough — but a window
closed with the X button may leave the handle open until the process is reaped.

### `bad OLVL magic` — the data image was not flashed

**Symptom.**

```
E (xxx) esp_fs: bad OLVL magic ffffffff — flash game data first
E (xxx) driver2: game data mount failed — flash gamedata.bin first
```

then nothing. `app_main` returns and the panel stays dark.

**Cause.** The check at `esp_fs.c:233-237` reads the first word of the mapped `gamedata`
partition and compares it to `'OLVL'` (`0x4C564C4F`). `ffffffff` means erased flash: the
application was flashed but the 10.95 MB data image was not.

This is easy to walk into because the two are flashed separately by design — the app is
674 KB and the data is 10.95 MB, so the normal edit loop only reflashes the app. Erase
the chip, or move to a new board, and the app comes back while the data does not.

**Fix.** Flash `gamedata.bin` at `0x410000` (§5).

**A magic that is neither `4c564c4f` nor `ffffffff`** means something else is at that
offset — usually a stale partition table from before `gamedata` moved. Reflash the
partition table *and* the data image, in that order. Note that changing an offset in
`partitions.csv` silently invalidates data already on the device: the bytes are still
there, just no longer where the new table says to look.
