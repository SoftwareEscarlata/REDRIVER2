# What was built

Driver 2 (PlayStation 1, 1999) runs on an ESP32-S3 microcontroller: a 240 MHz
dual-core Xtensa LX7 with 512 KB of internal SRAM, driving a 320x240 SPI panel.
It boots into the frontend, loads the Havana demo city, and is playable at
**16.8 fps**.

This document is the orientation. It says what that claim does and does not
mean, what actually works today, where the time goes, and how the tree is laid
out. The other numbered documents in this folder go into the rasterizer, the
dual-core split and the profiling in detail; this one deliberately does not.

---

## 1. What "Driver 2 on an ESP32-S3" actually means

It is **not** an emulator. There is no MIPS interpreter, no PSX BIOS, no
dynarec.

The base is [REDRIVER2](https://github.com/OpenDriver2/REDRIVER2), a complete
decompilation of the retail game into C. That C is cross-compiled for Xtensa
and runs natively on the S3. The PSX-specific parts of the program are served
by PsyCross ("PsyX"), a reimplementation of the PSX SDK — `LIBGPU`, `LIBETC`,
`LIBSPU`, `LIBCD`, and a software GTE. Upstream, PsyX's GPU backend translates
the PSX display list into OpenGL draw calls.

That backend is the part that could not come along. The port therefore consists
of two pieces of new work:

1. **A software rasterizer for the PSX primitive stream** — `PsyX_SoftRas.cpp`,
   about 1100 lines, added to PsyCross. It consumes the same ordering-table
   packets the real GPU would (`POLY_*`, `LINE_*`, `TILE`, `SPRT`, `DR_TPAGE`,
   `DR_TWIN`, `DR_AREA`) and writes 15-bit BGR555 pixels into the emulated
   1 MB VRAM array, with PSX semantics: affine attribute interpolation, CLUT
   lookup, the texture window formula, the four blend modes with per-texel STP
   bit, and reverse-ordering-table painter ordering — there is no depth buffer.
   It was developed on PC, where its output can be diffed against the GL
   backend, before it ever ran on the board.

2. **An ESP32 platform layer** — `esp32_port/`, an ESP-IDF v5.5 project. It
   supplies everything the SDL/OpenGL host used to: memory map, filesystem,
   pad, timing, present, logging.

The game logic itself is unmodified in any interesting way. Driver 2's physics,
AI, spooler and frontend are the decompiled originals, running as written.

| | PlayStation (1999) | ESP32-S3 (this port) |
|---|---|---|
| CPU | 33.9 MHz R3000A | 240 MHz Xtensa LX7 x2 |
| Main RAM | 2 MB | 8 MB octal PSRAM @ 80 MHz (+512 KB SRAM) |
| VRAM | 1 MB, dedicated hardware | 1 MB array in PSRAM |
| Geometry | GTE coprocessor | PsyCross's software GTE |
| Rasterizer | fixed-function GPU | `PsyX_SoftRas.cpp`, both cores |
| Output | 320x240 to a CRT | ST7789 320x240 over 80 MHz SPI |
| Storage | CD-ROM | 16 MB flash, memory-mapped |

---

## 2. What runs today

- **Boot to gameplay.** Power on, the frontend appears, you pick a drive, the
  Havana demo city spools in from flash, and you drive it.
- **The full render path.** Roads, buildings, foliage, traffic, the player car,
  shadows, the sky, the HUD, the frontend's hi-res 640x512 screens (rescaled
  onto the panel — see `esp_platform.cpp:88`).
- **Input**, three ways: physical buttons on header P1, single-key taps over the
  USB serial console, and full pad state streamed from a PC keyboard by
  `drive.ps1` at 50 Hz.
- **An optional texture minification filter**, toggled live with `f` on the
  serial console. It fixes the shimmering that a CRT used to low-pass away, and
  it is expensive; see §3.
- **On-device diagnostics**: a per-60-frame profile line, and serial dumps of
  the panel image, the whole VRAM, or a decoded texture page (`p`, `v`, `t`,
  `r`). All of the debugging was done with these, without a debugger.

### What does not run

| Not working | Why |
|---|---|
| **Audio** | Every `PsyX_SPUAL_*` entry point is a no-op (`esp_host.cpp:210-226`). Nothing drives I2S yet. The game is silent. |
| **Retail cities** | Only the free DRIVER2DEMO set is shipped. The full game's six cities do not fit in 16 MB of flash and would need a microSD reader. |
| **Attract demo / replays** | The demo data ships no `REPLAYS` folder. `FEmain.c` now checks `FileExists` before entering the idle demo, because without that the state machine retried the missing file forever. |
| **XA speech, FMV, subtitles** | Stubbed (`esp_host.cpp:236-248`). No audio and no video decoder. |
| **Persistent saves** | Writes go to PSRAM RAM-files that live for the session (`esp_fs.c:34-45`). They are lost on reset; NVS persistence is not written yet. |
| **Touch screen, IMU, microSD** | The panel's CST816D touch controller and the QMI8658 IMU are wired but unused. `PIN_SD_CS` is deliberately held high at boot so the card stays off the shared SPI bus (`display_esplcd.c:62`). |
| **Multiplayer, memory cards** | Never attempted. |

---

## 3. Performance, and what limits it

All figures measured on the device at 240 MHz.

| Milestone | Frame | fps | Raster |
|---|---|---|---|
| First frame that rendered | 200 ms | 5.0 | 153 ms |
| Span solver + 32-bit edge functions + local render state | 109 ms | 9.2 | 65 ms |
| CLUT cached per primitive | 106 ms | 9.4 | 63 ms |
| Pre-modulated CLUT, dead bounds masks, hoisted page pointer | 89 ms | 11.1 | 47 ms |
| Panel present moved to core 1 | 74 ms | 13.5 | 47 ms |
| Rasterisation split across both cores | 59 ms | **16.8** | 29 + 32 ms |
| With the texture filter on | 100-200 ms | 10 stationary, 5-9 driving | |

That table is easy to misread, so it is worth being explicit about who earned
what. The rasterizer rework — everything from the span solver down to the
pre-modulated CLUT — ends at **89 ms and 11.1 fps**, with raster time at 47 ms.
The step from there to 74 ms and 13.5 fps was not more rasterizer work at all:
it was moving the panel present onto core 1, which took its ~27 ms off the game
task while raster time stayed at 47 ms. Only then did splitting the
rasterisation itself across both cores cut that 47 ms to 29 ms on core 0 and
32 ms on core 1 — and since the frame waits for both, 32 + 27 = 59 ms.

Where the 59 ms goes:

| Stage | Time | Note |
|---|---|---|
| Rasterization | 32 ms | the slower of the two core bands; the other is 29 ms |
| Game logic + software GTE | 27 ms | transform, AI, physics, spooling |
| Panel present | 0 ms | 15-17 ms of SPI DMA, moved to core 1 |

The single biggest win was the inner loop: **221 → 68 cycles per covered
pixel**. That came from solving the three edge half-planes once per scanline
instead of testing coverage per pixel, caching and pre-modulating the 16-entry
CLUT per primitive, and passing the render state by reference from a
caller-local copy so the compiler stopped reloading it through a possible alias
with `vram[]`.

Two measurements shaped everything after that, and both are worth knowing
before proposing an optimization:

- **Texture fetch costs about 34 cycles per pixel** — half the remaining
  budget — measured by stubbing the fetch out.
- **Framebuffer writes to PSRAM are essentially free.** Redirecting them to
  internal SRAM changed the cost from 96 to 95 cycles per pixel. The loop is
  ALU-bound, not memory-bound. A band-rendering redesign (drawing into an SRAM
  tile and blitting out) was proposed and rejected on this evidence.

The dual-core split walks the *same* ordering table on both cores, each clipped
to a horizontal band of VRAM, so every pixel is written by exactly one core and
painter ordering holds within each band. It is verified byte-identical to the
single-core render: **0 of 19200 sampled pixels differ**. The split line is not
fixed at half — the top of a Driver 2 frame is sky and the bottom is road — so
it moves each frame toward whichever core drew fewer span pixels.

The texture filter exists because the PSX takes one texel per pixel, and a
2.8-inch LCD does not low-pass the result the way a CRT did. Measured on the
real primitive stream, a frame holds two very different populations:

| Geometry | Polygon size | Texels per pixel |
|---|---|---|
| Ground, buildings, foliage | 100-200 px | 1.2 - 1.6 |
| The player car | 2-3 px | 11 - 23 |

So the kernel scales with the compression ratio: 2x2 rotated-grid sampling past
2.5 texels/pixel, 4x4 past 4 but only on triangles whose **bounding box** is at
most 64 pixels — the test is literally
`(maxX - minX + 1) * (maxY - minY + 1) <= 64`, so it is a conservative
over-estimate of the covered area, which is the point. That gate is what makes
the 4x4 affordable, since the extreme ratios occur on the car's tiny polygons. It helps the scenery a great deal and the car much less; fixing
the car properly needs mip levels, which for 4bpp CLUT textures cannot be built
in index space and would have to be stored per palette. That is unresolved.

---

## 4. Repository tour

Two repositories, both on branch `esp32s3`: `SoftwareEscarlata/REDRIVER2`, with
`SoftwareEscarlata/PsyCross` as a submodule at `src_rebuild/PsyCross`.

### `esp32_port/` — top level

| Path | Purpose |
|---|---|
| `CMakeLists.txt`, `main/CMakeLists.txt` | ESP-IDF project. The component file is where the *build* of the port lives: which upstream sources are compiled, which are excluded, and the defines (`ESP32_PORT`, `PSYX_NO_RENDERER=1`, `USE_PGXP=0`, `SOFTRAS_PROFILE=1`). |
| `sdkconfig.defaults` | 240 MHz, 16 MB QIO flash, octal PSRAM at 80 MHz, 64 KB data cache with 64-byte lines, `-O2`. |
| `partitions.csv` | 4 MB app (the firmware is 674 KB), 11.94 MB `gamedata` to the end of flash. |
| `build.ps1` | cmake+ninja directly against ESP-IDF v5.5.4 at `C:/Espressif5.5`; no `idf.py`. (Its header comment and its closing "flash.ps1" hint are copy-paste leftovers from the sibling Descent port — flashing is done with `esptool` by hand.) |
| `get_demo_data.py` | Fetches the free DRIVER2DEMO set from the OpenDriver2 project's own browser-demo package. |
| `make_data.py` | Packs that set into the `OLVL` flash container: magic, count, 40-byte entries, then 4-byte-aligned blobs so mmapped pointers are usable in place. The image is 11,487,176 bytes — 10.95 MB — with sound banks, 7.40 MB without. |
| `drive.ps1` | Host-side keyboard driver. Reads real key up/down through `GetAsyncKeyState` and streams the complete pad word as `=HHHH` at 50 Hz. |

### `esp32_port/main/` — the platform layer

| File | What it is for |
|---|---|
| `esp_main.cpp` | `app_main`. Allocates the PSX memory map (ordering tables, prim tables, frontend/overlay/replay/sbank buffers) from PSRAM, mounts the data, brings up the panel, starts the present worker and the raster worker, then runs `redriver2_main` on a task pinned to core 0 with a 96 KB PSRAM stack. |
| `esp_platform.cpp` | Present and input. `SoftRas_PresentESP` (`:88`) picks the DISPENV rect out of `vram[]` and hands it to the display layer, which rescales whatever video mode the game asked for onto the fixed panel and converts BGR555 → RGB565. The R/B swap is **not** free: PSX VRAM puts red in the low bits, RGB565 puts it in the high bits, and the panel is configured `LCD_RGB_ELEMENT_ORDER_RGB` (`board_pins.h:25`), so the ST7789's MADCTL colour-order bit is clear and swaps nothing. The conversion moves red itself — `psx555toPanel()` in `display_esplcd.c:187`. Input merges GPIO buttons, serial taps and the host pad word. Also the serial debug dumps. |
| `esp_host.cpp` | The PsyX host surface: logging to serial, `PsyX_BeginScene`/`EndScene` (which is also where the profile line is printed), the vblank clock derived from `esp_timer`, publishing the pad report into the raw `PADRAW` buffers, and deliberate no-ops for CD, SPU, XA and FMV. Also defines the 1 MB `vram[]` in PSRAM. |
| `esp_raster.cpp` | Second-core rasterization: fork, join, the adaptive split line, and the barrier for primitives whose side effects reach outside a band. |
| `esp_stubs.cpp` | The remaining `GR_*` backend entry points. Most are empty, but `GR_ClearVRAM`, `GR_CopyVRAM` and `GR_ReadVRAM` are implemented for real — they are pure memory work, and `MoveImage` depends on them. |
| `esp_fs.c` / `esp_fs.h` | The `OLVL` container mounted as an ESP-IDF VFS at `/d`: reads come straight out of memory-mapped flash, name matching is case-insensitive and treats `\` and `/` alike, writes go to grow-on-demand PSRAM RAM-files. `esp_fs_map()` hands out a direct flash pointer. |
| `display_esplcd.c` / `display.h` | ST7789 driver over `esp_lcd`, shared with the OpenLara and Descent ports on this board. Chunked 555→565 conversion overlapping DMA, plus the async present task on core 1. `psx555toPanel()` (`:187`) is the pixel conversion: red up to bits 11-15, green widened 5→6 by replicating its top bit, then a byte swap because the panel wants MSB first. An earlier version left red in the low bits and claimed in a comment that MADCTL handled the swap; it did not, and the board ran for a long time with red and blue exchanged — an orange sky over blue asphalt. Nobody saw it because the serial screen dumps read `vram[]` directly and never pass through this function, so every debug image looked right. It was found by reading the code, not by looking at the panel. |
| `board_pins.h` | Waveshare ESP32-S3-Touch-LCD-2 pin map, verified against the vendor schematic. Includes the note that GPIO17 is driven low by the board and cannot be used as a button. |
| `esp_compat.h` | Force-included into every game and PsyX translation unit. Redirects `fopen` into `/d`, supplies `_stricmp`/`_mkdir`, and defines `D2_PSRAM`. |
| `SDL.h`, `SDL_*.h` | A shim. Game code includes `<SDL.h>` for message boxes and a delay; message boxes go to the serial log. |

### Upstream files that were modified

Everything else in both trees is untouched. The changes are small and each one
has a reason.

**REDRIVER2** (18 files, +75/-24 against upstream `b2d88574`):

| File | Change |
|---|---|
| `Game/C/system.c` | The 2 MB PSX RAM arena `g_allocatedMem` moves to PSRAM `.bss`; `ResetCityType` no longer `free()`s the spool pointer, which now points into flash. |
| `Game/C/spool.c` | The level spooler's `malloc(16 MB)` cannot succeed here. Every reader only `memcpy`s out of that pointer, so the memory-mapped level file is used in place via `esp_fs_map` — zero RAM. |
| `Game/Frontend/FEmain.c` | `FileExists` check before entering the idle demo (no `REPLAYS` on the demo disc). |
| `Game/C/glaunch.c` | A failure path in `State_GameStart`: without a `SetState` call the state re-entered and retried the missing replay forever, leaving the please-wait screen up. |
| `Game/C/handling.c` | `ProcessCarPad` walked through a null pointer whenever the rubber-banding chase target's cosmetics had never been spooled in — which the demo data guarantees. |
| `cars.c`, `cell.c`, `pathfind.c`, `pres.c`, `shadow.c`, `FEmain.c` | `D2_PSRAM` on large cold arrays, so they land in PSRAM `.bss` instead of the 512 KB of internal SRAM. |
| `Game/driver2.h` | `trap()` no longer emits an x86 `int3`. |
| `premake5*.lua` | `USE_PGXP=0` for the PC build too, so PC and device build the same configuration. |

**PsyCross** (7 files, +1301/-7 against upstream `e56e4cd`):

| File | Change |
|---|---|
| `src/gpu/PsyX_SoftRas.cpp` / `.h` | New. The rasterizer. |
| `src/gpu/PsyX_GPU.cpp` / `.h` | Dispatch primitives to the rasterizer; `DRAWENV` becomes per-core `g_drawEnv[2]`; the ordering-table walk is split out as `SR_WalkOT` so both cores can run it; barriers around `MoveImage` and CLUT uploads; the unused GL vertex/split buffers shrink and move to PSRAM. |
| `src/PsyX_main.cpp` | Present directly from `vram[]` instead of reading back a GL framebuffer. |
| `include/PsyX/PsyX_render.h` | `PSYX_NO_RENDERER` mode: keep the `GR_*` API surface with no GL backend. |
| `src/pad/PsyX_pad.cpp` | Input diagnostics used while debugging on PC. |

---

## 5. State of things

| Subsystem | Status | Detail |
|---|---|---|
| **Video** | Working | Software rasterizer on both cores into a 1 MB PSRAM VRAM; ST7789 present on core 1. 16.8 fps in game, about 10 stationary (5-9 driving) with the optional filter. All PSX primitive types, blend modes, texture formats and the texture window. |
| **Input** | Working | GPIO buttons on header P1, serial taps, and full pad state from `drive.ps1`. GPIO17 is unusable on this board and is auto-disabled at boot. No analogue stick, no vibration, no touch. |
| **Filesystem** | Working, read-only | `OLVL` container in memory-mapped flash behind a VFS at `/d`. Reads cost zero RAM. No directories, so `FS_FindFirst`/`FindNext` always come up empty. |
| **Game data** | Demo only | The free DRIVER2DEMO set: Havana, the frontend, one mission, the sound banks. 10.95 MB (11,487,176 bytes) of an 11.94 MB partition. Retail cities need a microSD reader that does not exist yet. |
| **Audio** | Not started | All SPU calls are no-ops that report voices idle so the mixer-driven game logic keeps advancing. XA speech and music are stubbed. The banks ship anyway — the frontend blocks on a failed `VOICES2.BLK` open. |
| **Save data** | Session only | Config and progress writes land in PSRAM RAM-files and are lost on reset. NVS persistence is designed for but not written. |
| **Debug/tooling** | Working | Per-60-frame profile line (per-core raster ms, present, other, tris, kpx, cycles/px, divergence canary, worker stack headroom), serial screen/VRAM/texture-page dumps, runtime kill switches for the fork and the filter. |

---

## 6. Where to look next

- The **rasterizer** document: the pixel kernels, the undefined-behaviour bug
  that poisoned every row of a primitive, and how 221 cycles per pixel became
  68.
- The **dual-core** document: why walking the same ordering table twice is
  sound, what had to become per-core, and the two primitives per frame that
  need a rendezvous.
- The **performance** document: the full profile methodology and the
  measurements that killed the band-rendering idea.
- The **build and flash** document: toolchain setup, data packing, and getting
  the two partitions onto the board.

The git history of both repositories is the primary record. Every commit
message explains the reasoning, not just the change:

```
git -C E:/Hardware/Driver2/REDRIVER2 log --format=%B
git -C E:/Hardware/Driver2/REDRIVER2/src_rebuild/PsyCross log --format=%B
```
