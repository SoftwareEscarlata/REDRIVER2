# The ESP32 platform layer

REDRIVER2 is a decompilation of the PlayStation game, built on top of **PsyCross**
("PsyX"), a reimplementation of the PSX SDK whose reference backend is SDL + OpenGL.
Porting the game to an ESP32-S3 therefore means replacing two things: the *renderer*
(covered elsewhere — a software rasterizer that writes the emulated VRAM array
directly) and the *host layer* — files, memory, panel, input, timing.

This document covers the second half: everything in `esp32_port/`. It is the layer
that makes a 240 MHz microcontroller with 512 KB of internal SRAM look enough like a
PC host that ~150 unmodified game translation units link and run against it.

The layer is small on purpose. Six files:

| File | Lines | Responsibility |
|---|---:|---|
| `esp32_port/main/esp_main.cpp` | 122 | `app_main`, PSX memory map, boot order, game task |
| `esp32_port/main/esp_fs.c` | 280 | OLVL flash container, VFS at `/d`, RAM write-files, `esp_fs_map` |
| `esp32_port/main/display_esplcd.c` | 305 | ST7789 over SPI, DMA chunking, 555→565, rescale, async present |
| `esp32_port/main/esp_platform.cpp` | 352 | present entry point, GPIO + serial pad, debug dumps, clock |
| `esp32_port/main/esp_host.cpp` | 254 | PsyX host services: frame, pad publish, vblank, log, CD/SPU/XA no-ops |
| `esp32_port/main/esp_stubs.cpp` | 123 | `GR_*` backend surface; VRAM copy/read/clear implemented for real |

Plus three headers (`esp_fs.h`, `display.h`, `board_pins.h`, `esp_compat.h`), the
partition table, and `make_data.py` which builds the flash data image.

---

## 1. The read-only filesystem

### 1.1 Why a custom filesystem at all

The game reads its data with plain `fopen`/`fread`. On the PC that is the host OS; on
the PlayStation it is the CD. On this board there is neither. The data set is ~11 MB
and the flash is 16 MB, so the obvious answer is "put it in flash" — but SPIFFS or
FAT would mean a real allocator, real block I/O, and a RAM buffer for every read.

The port instead uses the trick the ESP32-S3 makes cheap: **memory-map the data
partition** and let the MMU + cache do the paging. A read becomes a `memcpy` from a
pointer, and a *bulk* read can become no copy at all.

### 1.2 The OLVL container

`esp32_port/make_data.py` packs a directory tree into a single flat image. The format
is shared with the OpenLara and Descent ports on the same board:

```
u32 magic  = 'OLVL' (0x4C564C4F)
u32 count
count × { char name[32]; u32 offset; u32 size }
... 4-byte-aligned file blobs ...
```

Two properties matter:

* **Names keep their relative path with `/` separators** and are capped at 31
  characters (`make_data.py:43-44` hard-errors if a path is too long), so
  `LEVELS/HAVANA.LEV` is the key.
* **Blobs are 4-byte aligned** (`make_data.py:61,68`) precisely so an mmap'd pointer
  into the middle of the image is usable in place, which section 1.6 depends on.

The packer drops `GFX/HQ` (4.7 MB of PC-only high-resolution font TGAs — the two
`.tga` files alone are 2.3 MB each) and can optionally drop the sound banks. The
splash TIMs are explicitly *not* droppable; an early revision cut them and the boot
sequence blocked forever, which is recorded in both the script's comment
(`make_data.py:24`) and the partition table's.

For the free demo set the result is:

```
gamedata.bin: 11,487,176 bytes (10.96 MB) with sound
  LEVELS/HAVANA.LEV     5.98 MB
  SOUND/VOICES2.BLK     2.49 MB
  SOUND/MUSIC.BIN       0.75 MB
  DATA/GFX.RAW          0.38 MB
  DATA/CARCONT.RAW      0.38 MB
```

### 1.3 Mount: one `esp_partition_mmap`

```c
// esp_fs.c:220-241
const esp_partition_t* part = esp_partition_find_first(
    ESP_PARTITION_TYPE_DATA, 0x40, "gamedata");
...
esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &map, &h);
...
sBase  = (const uint8_t*)map;
sCount = hdr[1];
sFiles = (const OlvlEntry*)(sBase + 8);
```

`0x40` is a private data subtype — the image is not a filesystem ESP-IDF knows about,
so it gets its own tag. After this call the entire 11.94 MB partition is visible as
ordinary memory in the DROM window, backed by the 32 KB instruction / 64 KB data
cache. Nothing is copied. Boot cost is one `mmap` and a magic check; if the magic is
wrong the mount fails with an explicit "flash game data first" message rather than
crashing later on garbage (`esp_fs.c:234-237`).

### 1.4 Registering as a VFS at `/d`

```c
// esp_fs.c:243-253
esp_vfs_t vfs = { 0 };
vfs.flags  = ESP_VFS_FLAG_DEFAULT;
vfs.open = vfs_open;   vfs.read  = vfs_read;   vfs.write = vfs_write;
vfs.lseek = vfs_lseek; vfs.close = vfs_close;
vfs.fstat = vfs_fstat; vfs.stat  = vfs_stat;   vfs.unlink = vfs_unlink;
return esp_vfs_register("/d", &vfs, NULL) == ESP_OK ? 0 : -1;
```

Eight callbacks, no directory support. The open-file table is a fixed array of 12
slots (`MAX_OPEN`), which is ample: the game opens a file, reads it, and closes it.

The game is pointed at this mount by a single macro in the force-included compat
header:

```c
// esp_compat.h:23
#define fopen(name, mode) ((FILE*)esp_game_fopen((name), (mode)))
```

and `esp_game_fopen` prefixes `/d/` to any relative path:

```c
// esp_fs.c:267-280
void* esp_game_fopen(const char* name, const char* mode)
{
    FILE* f;
    if (name[0] == '/') {
        f = fopen(name, mode);
    } else {
        char buf[80];
        snprintf(buf, sizeof(buf), "/d/%s", name);
        f = fopen(buf, mode);
    }
    ...
}
```

Note that `esp_fs.c` is deliberately **not** in the force-include list — the
`-include esp_compat.h` flag is applied only to `${GAME_SOURCES}`
(`main/CMakeLists.txt:77-78`). So the `fopen` inside `esp_game_fopen` is the real
newlib one, and there is no infinite recursion.

`gDataFolder` is cleared at startup (`esp_main.cpp:87`) because the container already
stores paths relative to the data root, so the game's own prefixing produces exactly
the container key.

### 1.5 Name matching: case-insensitive and separator-agnostic

Driver 2 is a DOS/PSX-era codebase. It builds paths like `LEVELS\HAVANA.LEV`
(`system.c:143-146`) and mixes case freely. The container stores whatever the host
filesystem reported. Rather than normalizing at pack time and hoping, matching is done
character by character:

```c
// esp_fs.c:63-74
static int nameEq(const char* a, const char* b)
{
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca == '\\') ca = '/';
        if (cb == '\\') cb = '/';
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}
```

ASCII-only lowercase folding plus `\` → `/`. Both operands are normalized, so it does
not matter which side is which. Lookup is a linear scan over `sCount` entries
(`flashFind`, `esp_fs.c:76-81`); with a few dozen files and a file open happening at
most a handful of times per level load, an index would be optimizing nothing.

The game also normalizes on its own via `FS_FixPathSlashes`, which the port
reimplements in `esp_stubs.cpp:40-44` (the upstream `utils/fs.cpp` is excluded from
the build because it needs POSIX `glob()`).

### 1.6 `esp_fs_map`: the level spooler reads city data in place

This is the single most important function in the file, and it exists because of one
line in the upstream spooler:

```c
// spool.c:1447-1448 (PC path)
SpoolLumpSize = 16 * 1024 * 1024;   // allocate 16 MB of RAM for spoolable data
g_CurrentLevelSpoolData = (char*)malloc(16 * 1024 * 1024);
```

Sixteen megabytes, on a board with eight. The port does not shrink the buffer — it
removes it:

```c
// spool.c:1434-1451
#ifdef ESP32_PORT
    // The 16MB copy below does not fit on this target. The level file is
    // already memory-mapped from flash, so use it in place: every reader
    // below only memcpy's out of this pointer.
    const void* levelBase = esp_fs_map(g_CurrentLevelFileName, NULL);
    if (levelBase == NULL)
        return 0;
    g_CurrentLevelSpoolData = (char*)levelBase + SpoolLumpOffset;
#else
    ...
    loadsectors(g_CurrentLevelSpoolData, SpoolLumpOffset / CDSECTOR_SIZE, SpoolLumpSize);
#endif
```

This is sound because of how the spooler consumes the pointer. Every access below is
of the form

```c
// spool.c:1460-1465
#define SPL_READ(dest, nsectors) \
    { int readSize = (nsectors)*CDSECTOR_SIZE; \
      memcpy(dest, spoolDataPtr, readSize); \
      spoolDataPtr += readSize; }
```

— a `memcpy` *out of* the lump into a small destination buffer, with the source
address computed as `g_CurrentLevelSpoolData - SpoolLumpOffset + sector*2048`
(`spool.c:1489`). Nothing writes through the pointer, and nothing needs it contiguous
with anything else. So the "buffer" can be a window onto flash.

`esp_fs_map` itself is nine lines:

```c
// esp_fs.c:256-265
const void* esp_fs_map(const char* name, unsigned int* size)
{
    if (name[0] == '/') name++;
    if (!strncmp(name, "d/", 2)) name += 2;
    const OlvlEntry* e = flashFind(name);
    if (!e) return NULL;
    if (size) *size = e->size;
    return sBase + e->offset;
}
```

The two leading-path strips exist because the caller may hand it either the raw game
path (`LEVELS/HAVANA.LEV`, which is what `g_CurrentLevelFileName` holds after
`FS_FixPathSlashes`) or a VFS path. It is declared in `esp_compat.h:18` as well as
`esp_fs.h`, so game code that has never heard of the ESP32 port can call it behind an
`#ifdef ESP32_PORT` without a new include.

**What this bought:** 16 MB of RAM that does not exist, replaced by zero. The cost is
that streaming reads now go through the flash cache — a cache miss on a 64-byte line
at 80 MHz QIO. Since the spooler was reading from a `malloc`'d PSRAM buffer in the
alternative, and PSRAM is also 80 MHz octal behind the same data cache, the two are
comparable; the difference is that one of them is impossible.

### 1.7 RAM-backed write files

The container is read-only, but the game writes: `config.dat`, `progress.dat`, replay
files. Writes land in `MAX_RAM_FILES = 12` grow-on-demand PSRAM buffers:

```c
// esp_fs.c:148-165
static ssize_t vfs_write(int fd, const void* src, size_t len)
{
    ...
    uint32_t need = f->pos + len;
    if (need > rf->cap) {
        uint32_t cap = rf->cap ? rf->cap : 1024;
        while (cap < need) cap *= 2;
        uint8_t* nd = heap_caps_realloc(rf->data, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!nd) { errno = ENOSPC; return -1; }
        rf->data = nd; rf->cap = cap;
    }
    memcpy(rf->data + f->pos, src, len);
    ...
}
```

Doubling from 1 KB, always from PSRAM (`MALLOC_CAP_SPIRAM`) so a large replay cannot
eat the scarce internal heap.

The RAM files **shadow** the container: `vfs_open` checks `ramFind` before
`flashFind` on the read path (`esp_fs.c:106-111`), so a config the session has written
wins over a shipped one. Opening for write with `O_APPEND` clear truncates
(`esp_fs.c:122`), matching `fopen(..., "wb")` semantics.

Directory creation is defined away — `#define _mkdir(p) (0)` (`esp_compat.h:35`) —
because the VFS has no directories at all; `LEVELS/HAVANA.LEV` is a *name*, not a
path.

**Honest limitation.** RAM files live for the session only; there is no NVS write-back
yet (`esp_fs.c:4-5` says as much). And for the profile specifically, persistence does
not currently reach this mechanism at all. The game builds the path as:

```c
// loadsave.c:89-106, 139-140
homepath = getenv(HOME_ENV);       // esp_compat.h:34 -> "HOME"
if (homepath) { strcpy(str, homepath); strcat(str, "/.driver2"); _mkdir(str); }
else          { str[0] = 0; }
...
strcat(filePath, "/config.dat");
```

On ESP-IDF newlib the environment is empty, so `getenv("HOME")` is `NULL`, `str` is
empty, and `filePath` ends up as the **absolute** `"/config.dat"`. `esp_game_fopen`
takes its absolute branch and calls `fopen("/config.dat")`, which matches no
registered VFS prefix (only `/d` exists) and fails with `ENOENT`. Configuration and
progress saving are therefore silently no-ops today. Relative paths — which is what
the replay code uses — do go through `/d` and do hit the RAM files. The fix is one
line in either `GetGameProfilePath` or `esp_game_fopen`; it has not been needed yet
because there is nothing worth saving until audio and a menu pass land.

Directory *enumeration* is likewise stubbed: `FS_FindFirst` returns `NULL`
(`esp_stubs.cpp:46-51`), so wildcard searches always come up empty. That is why the
attract-demo state machine needed an explicit `FileExists` check before entering
`GameStart` — with no `REPLAYS` folder, `LoadAttractReplay` failed, nothing called
`SetState`, and the state re-entered and retried forever behind the please-wait
screen.

---

## 2. Memory placement

### 2.1 The budget

| Resource | Size | Notes |
|---|---:|---|
| Internal SRAM | 512 KB | shared between IRAM (code) and DRAM (data) |
| Octal PSRAM | 8 MB | 80 MHz, behind the 64 KB data cache, 64-byte lines |
| Flash | 16 MB | QIO 80 MHz; 4 MB app + 11.94 MB data |

Everything hot must be internal; everything large must be external. The interesting
part of the port is deciding which arrays are which, and doing it without editing 150
game files.

### 2.2 `D2_PSRAM` / `EXT_RAM_BSS_ATTR`

ESP-IDF provides `EXT_RAM_BSS_ATTR`, a section attribute that places a zero-initialized
global in `.ext_ram.bss` — PSRAM rather than internal `.bss`. It requires
`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`, which `sdkconfig.defaults:21` sets.

The port exposes it to the game under a neutral name, defined in the header that is
force-included into every game translation unit:

```c
// esp_compat.h:37-41
// Marks a large, cold game array for octal PSRAM .bss instead of internal
// SRAM (see the port notes). Defined here because this header is force-
// included into every game translation unit.
#include "esp_attr.h"
#define D2_PSRAM EXT_RAM_BSS_ATTR
```

The consequence is that annotating a game global costs exactly one token at the
declaration and nothing anywhere else — no `#ifdef`, no include, no pointer
indirection, no change to any use site:

```c
// cars.c:76
D2_PSRAM SVECTOR gTempCarVertDump[MAX_CARS][MAX_DENTING_VERTS];
```

Ten game arrays carry it. Where the file is not force-included (PsyCross, and
`system.c`'s arena, which is inside an `#ifdef ESP32_PORT` block anyway) the raw
`EXT_RAM_BSS_ATTR` is used directly.

Here is what actually landed in PSRAM `.bss`, read out of the link map
(`build/driver2_esp32s3.map`, `.ext_ram.bss` at `0x3c090000`):

| Symbol | Size | Where declared |
|---|---:|---|
| `vram` | 1,048,576 | `esp_host.cpp:254` |
| `g_allocatedMem` | 2,097,152 | `system.c:122` |
| `carPolyBuffer` | 57,624 | `cars.c:97` |
| `gTempCarVertDump` | 40,960 | `cars.c:76` |
| `distanceCache` | 32,768 | `pathfind.c:33` |
| `track_buffer` | 28,672 | `shadow.c:42` |
| `gHiresFontCharData` | 25,088 | `pres.c:26` |
| `gHiresFEFontCharData` | 25,088 | `FEmain.c:41` |
| `PsxScreens` | 20,328 | `FEmain.c:321` |
| `cell_object_buffer` | 16,384 | `cell.c:8` |
| `gTempLDCarUVDump` | 5,120 | `cars.c:80` |
| `g_vertexBuffer` | 1,280 | `PsyX_GPU.cpp:70` |
| `g_splits` | 576 | `PsyX_GPU.cpp:71` |
| **total** | **3,399,616** (3.24 MiB) | |

Two entries deserve comment.

**`vram`** is the emulated PlayStation frame buffer, 1024 × 512 × 16 bits = 1 MB. It
is the single hottest array in the system — the rasterizer writes it and samples
textures out of it — and it is in PSRAM. That looks wrong and is not:

> Framebuffer writes to PSRAM are essentially free. Redirecting them to internal
> SRAM changed nothing: 96 versus 95 cycles per pixel. The rasterizer's inner loop
> is ALU-bound, not memory-bound.

That measurement is why a band-rendering redesign (render into an internal SRAM tile,
blit out) was considered and rejected. It would have added complexity to buy back a
cost that was not being paid.

**`g_vertexBuffer` / `g_splits`** are PsyCross's OpenGL batching buffers. They are
never exercised here — the software rasterizer writes `vram[]` directly — but the
split bookkeeping still references them, so they must exist. Rather than `#ifdef` out
their users, PsyCross shrinks them for this target:

```c
// PsyX_render.h:118-125
#ifdef ESP32_PORT
// The GL vertex path is never exercised (the software rasterizer writes vram[]
// directly), so this buffer only needs to exist, not to be big.
#define MAX_VERTEX_BUFFER_SIZE  64
#else
#define MAX_VERTEX_BUFFER_SIZE  (1 << (sizeof(ushort) * 8))
#endif
```

`65536 → 64` entries and `MAX_DRAW_SPLITS 4096 → 4` (`PsyX_GPU.h`), which is how
1.9 MB became 1,856 bytes. They go to PSRAM as well, because "small enough not to
matter" is not the same as "worth internal SRAM".

### 2.3 The PSX memory arena

Driver 2 does not use `malloc` for game data. It has a PSX-style bump allocator over a
fixed arena, and on PC that arena is a 2 MB static array. On this target the array is
the same array, in PSRAM:

```c
// system.c:118-127
#elif !defined(PSX)
#ifdef ESP32_PORT
#include "esp_attr.h"
EXT_RAM_BSS_ATTR char g_allocatedMem[0x200000];   // 2MB PSX RAM arena -> PSRAM
#else
char g_allocatedMem[0x200000];
#endif
volatile char* mallocptr = g_allocatedMem;
volatile char* malloctab = g_allocatedMem;
```

Nothing else changes: `mallocptr`/`malloctab` still bump through it, and every
`sys_malloc` in the game is untouched. This is the whole reason the arena approach is
pleasant to port — there is exactly one definition to move.

### 2.4 The buffers `esp_main.cpp` allocates, and why the sizes are copied verbatim

On PC these ten buffers are either statics or `malloc`s in
`src_rebuild/redriver2_psxpc.cpp` — a host file this build does not compile. So the
port has to provide them. `esp_main.cpp:44-59` does, from PSRAM:

```c
static void* psram(size_t size, const char* what)
{
    void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) ESP_LOGE(TAG, "PSRAM alloc failed: %s (%u KB)", what, (unsigned)(size >> 10));
    return p;
}

static bool allocPsxMemory()
{
    _overlay_buffer  = (char*)psram(0x50000, "overlay");
    _frontend_buffer = (char*)psram(0x60000, "frontend");
    ...
}
```

| Buffer | ESP32 (`esp_main.cpp`) | PC (`redriver2_psxpc.cpp:478-487`) | Bytes |
|---|---|---|---:|
| `_overlay_buffer` | `psram(0x50000)` | `malloc(0x50000)` | 327,680 |
| `_frontend_buffer` | `psram(0x60000)` | `malloc(0x60000)` | 393,216 |
| `_other_buffer` | `psram(0x50000)` | `malloc(0x50000)` | 327,680 |
| `_other_buffer2` | `psram(0x50000)` | `malloc(0x50000)` | 327,680 |
| `_OT1` | `OTSIZE * sizeof(OTTYPE)` | `OTSIZE * sizeof(OTTYPE)` | 65,536 |
| `_OT2` | `OTSIZE * sizeof(OTTYPE)` | `OTSIZE * sizeof(OTTYPE)` | 65,536 |
| `_primTab1` | `PRIMTAB_SIZE` | `PRIMTAB_SIZE` | 327,680 |
| `_primTab2` | `PRIMTAB_SIZE` | `PRIMTAB_SIZE` | 327,680 |
| `_sbank_buffer` | `psram(0x80000)` | `malloc(0x80000)` | 524,288 |
| `_replay_buffer` | `psram(0x50000)` | `malloc(0x50000)` | 327,680 |
| | | **total** | **3,014,656** |

(`OTSIZE` is `0x2000` and `OTTYPE` is `unsigned long long` on a 32-bit non-x64 target
— `system.h:139`, `psyx_compat.h:33`; `PRIMTAB_SIZE` is `0x50000` with
`USE_EXTENDED_PRIM_POINTERS=1` — `system.h:143`.)

**The sizes are identical to the PC build, deliberately.** It is tempting on a
memory-constrained target to trim buffers that "look" oversized. Every one of these is
indexed by game code with hardcoded offsets — `_other_buffer + modelNumber * 0x10000 +
(type-1) * 0x4000` in `cars.c:974`, `_overlay_buffer[524]` in `E3stuff.c:134`,
`LoadfileSeg(filename, _overlay_buffer, 20, 0x4ff80)` in `E3stuff.c:128`. Shrinking
any of them turns a working game into a memory corruption bug that reproduces only in
one mission. The port pays 2.9 MB of PSRAM to keep the layout bit-compatible with the
reference build, and that is a good trade.

### 2.5 The 96 KB PSRAM game-task stack

```c
// esp_main.cpp:112-121
// big stack in PSRAM (the PSX code recurses through the renderer);
// static creation is required for an external stack
static StaticTask_t tcb;
const uint32_t stackBytes = 96 * 1024;
StackType_t* stack = (StackType_t*)heap_caps_malloc(stackBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
if (!stack) { ESP_LOGE(TAG, "no PSRAM for game stack"); return; }
xTaskCreateStaticPinnedToCore(gameTask, "game", stackBytes, NULL, 5, stack, &tcb, 0);
```

Three decisions in nine lines:

* **96 KB.** The PSX renderer recurses through the scene graph and the frontend
  builds deep call chains; 96 KB is generous but it is PSRAM, where being generous is
  cheap.
* **`xTaskCreateStaticPinnedToCore`, not `xTaskCreatePinnedToCore`.**
  `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y` (`sdkconfig.defaults:22`) permits an
  external stack, but the dynamic creation path always takes its stack from internal
  memory. The only way to *place* a stack in PSRAM is to allocate it yourself and hand
  it to the static variant.
* **Pinned to core 0.** Core 1 belongs to the present task and the second rasterizer
  band.

The counterpart is the rule for tasks whose stack must **not** be external:

```c
// esp_raster.cpp:62-68
// Internal stack: an ISR runs on the interrupted task's stack on Xtensa,
// and the panel DMA completion handler is IRAM-resident precisely so it can
// fire with the flash cache disabled — at which point a PSRAM stack is
// unreachable.
// 6 KB: measured high-water use is under 2 KB, and the rest is headroom for
// an interrupt, which on Xtensa runs on the interrupted task's stack.
xTaskCreatePinnedToCore(rasterWorker, "raster1", 6144, NULL, 6, &sWorker, 1)
```

This is the ESP32's sharpest trap and it is worth stating plainly: on Xtensa an
interrupt handler executes on the stack of whatever task it interrupted. If that stack
is in PSRAM and the interrupt can fire while the cache is disabled (as flash-write and
some driver paths do), the handler faults. So the rasterizer worker and the present
task keep internal stacks (6 KB and 3 KB), and only the game task — which is never the
target of a cache-disabled ISR in this design — gets the cheap external one. The
worker's actual usage is reported every 60 frames by `SoftRas_WorkerStackFree()`
(`esp_raster.cpp:157-160`, printed at `esp_host.cpp:109`).

### 2.6 What stays internal

| Item | Size | Why |
|---|---:|---|
| `.iram0.text` | 66,391 B | ISR-safe code, cache-disabled paths |
| `.dram0.data` | 45,816 B | initialized globals |
| `.dram0.bss` | 206,424 B | everything not marked `D2_PSRAM` |
| DMA chunk buffers ×2 | 20,480 B | `MALLOC_CAP_INTERNAL \| MALLOC_CAP_DMA` — DMA cannot source from PSRAM here |
| raster worker stack | 6,144 B | ISR-on-task-stack rule |
| present task stack | 3,072 B | same |
| main task stack | 8,192 B | `CONFIG_ESP_MAIN_TASK_STACK_SIZE` |

Static figures are from the link map; the *runtime* free figure is printed at boot by
the game task itself, alongside the largest free block and PSRAM free
(`esp_main.cpp:67-70`), which is the number to watch when adding anything.

Adding it up: PSRAM holds 3.24 MiB of `.bss`, 2.87 MiB of explicit buffers and 96 KB
of stack — about 6.2 MiB committed of 8 MiB, before the PSRAM heap serves RAM files
and anything else. It is comfortable but not roomy, which is the reason `GFX/HQ` never
ships and audio buffers are still an open question.

---

## 3. The display pipeline

### 3.1 The panel and the pin map

`board_pins.h` is the single source of truth, verified against the vendor schematic
(`ESP32-S3-Touch-LCD-2-SchDoc.pdf`) and Waveshare's own ESP-IDF demos.

| Signal | GPIO | Notes |
|---|---:|---|
| LCD SCLK | 39 | SPI2, 80 MHz |
| LCD MOSI | 38 | |
| LCD CS | 45 | |
| LCD DC | 42 | |
| LCD RST | — | no GPIO; RC power-on reset, software `SWRESET` only |
| LCD backlight | 1 | SS8050 NPN driver, active high |
| microSD MISO | 40 | belongs to the *bus* config, never to the panel IO |
| microSD CS | 41 | held high from boot so the card stays off the shared bus |
| Touch SDA / SCL / INT | 48 / 47 / 46 | CST816D at `0x15`, I2C0 shared with the QMI8658 IMU |
| Button UP | 2 | P1 pin 1 |
| Button DOWN | 4 | P1 pin 2 |
| Button LEFT | 6 | P1 pin 3 |
| Button RIGHT | 16 | P1 pin 4 — on-board 4.7K pull-up (camera SCCB) |
| Button CROSS | 17 | P1 pin 5 — **unusable, driven low by the board** (§4.2) |
| Button SQUARE | 18 | P1 pin 6 |
| Button L1 | 21 | P1 pin 7 — on-board 4.7K pull-up |
| Button R1 | 8 | P1 pin 8 |
| Button START | 7 | P1 pin 9 |
| Button SELECT | 10 | P1 pin 10 |
| BOOT (extra CROSS) | 0 | |

All buttons are on header P1, active low, common ground on P1 pin 13. Most of these
are DVP camera pins, so the camera module must stay unplugged.

The panel is an ST7789T3, natively 240×320 portrait. Landscape is Waveshare's
"rotation 1", which the ESP-IDF driver expresses as `swap_xy(true)` +
`mirror(X=true, Y=false)` (`display_esplcd.c:97-99`). Colour inversion is on because
the panel is IPS.

### 3.2 Chunked DMA with ping-pong buffers

There is no room for a 150 KB framebuffer in internal SRAM, and DMA on this path
cannot source from PSRAM. So the frame is converted and transmitted in horizontal
bands:

```c
// display_esplcd.c:33-34
#define CHUNK_LINES 16
#define CHUNK_BYTES (DISPLAY_WIDTH * CHUNK_LINES * 2)     // 10,240 bytes
```

Two such buffers are allocated `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA`
(`display_esplcd.c:108-114`), 20 KB total. The synchronization is a **counting
semaphore initialized full**, which is a neater formulation than the usual
"flag + wait":

```c
// display_esplcd.c:103-106
// counts FREE ping-pong buffers: take one before writing into a buffer,
// each on_color_trans_done returns one. Guarantees we never overwrite a
// buffer whose DMA transfer is still in flight.
sTransDone = xSemaphoreCreateCounting(2, 2);
```

The producer takes a permit *before* writing (`xSemaphoreTake` at
`display_esplcd.c:289`), the completion ISR gives one back
(`onTransDone`, `display_esplcd.c:43-49`). Two permits means one buffer can be filled
while the other is on the wire, and the loop can never race ahead. `displayWaitFlush`
drains both permits and returns them, which is exactly "no transfer in flight".

The numbers work out cleanly. A 10,240-byte chunk at 80 MHz is 1.02 ms on the wire;
converting the next chunk's 5,120 pixels at roughly ten instructions each is about
0.21 ms. Conversion hides entirely inside the previous transfer, and the frame costs
15 transfers ≈ 15.4 ms — which matches the measured 17 ms present almost exactly and
is why §3.5 exists. `max_transfer_sz` is set to `CHUNK_BYTES`
(`display_esplcd.c:70`), well under the S3's 32,768-byte per-transaction cap, so each
band is one transaction and per-transaction overhead is negligible.

The 16-line choice is inherited from the Descent port on the same board (48 lines in
OpenLara, where internal SRAM was less contested): 20 KB of DMA buffers instead of
60 KB, at a cost of roughly 0.4 ms per frame in extra transaction overhead
(`display_esplcd.c:27-32`).

### 3.3 555 → 565, and the MADCTL question

PSX VRAM is BGR555: bit 15 is the semi-transparency flag, bits 14–10 blue, 9–5 green,
4–0 red. The rasterizer writes exactly that (`PsyX_SoftRas.cpp:189`,
`(r | (g << 5) | (b << 10))`). The panel wants 16 bits per pixel with six of them for
green. The conversion is four ALU ops:

```c
// display_esplcd.c:296-299
uint16_t c = s[sx >> 16];
uint16_t g5 = (c >> 5) & 0x1F;
uint16_t v = (uint16_t)((c & 0x1F) | (((g5 << 1) | (g5 >> 4)) << 5) | (((c >> 10) & 0x1F) << 11));
d[x] = (uint16_t)((v >> 8) | (v << 8));
```

Green is widened by *replication* — `(g5 << 1) | (g5 >> 4)` copies the top bit into
the new low bit, so 0x1F maps to 0x3F rather than 0x3E and full-scale stays full
scale. The final byte swap compensates for little-endian memory versus MSB-first SPI;
the panel receives `v` exactly as written.

Note what is **not** done: red stays in the low five bits and blue in the high five.
The word handed to the panel is BGR565, not RGB565. The intent, stated in
`display.h:27-29` and in the original commit message, is that the ST7789's MADCTL BGR
bit performs the red/blue swap in hardware, for free, so the CPU never touches it.

**This is worth flagging honestly, because the configuration as committed does not
enable that bit.** `board_pins.h:25` declares:

```c
#define LCD_RGB_ORDER    LCD_RGB_ELEMENT_ORDER_RGB
```

and ESP-IDF's ST7789 driver maps that to `madctl_val = 0`, i.e. `LCD_CMD_BGR_BIT`
(`1 << 3`) clear — RGB order
(`components/esp_lcd/src/esp_lcd_panel_st7789.c:84-90`). Worse, the *other*
conversion path in the same file disagrees with this one: the 8bpp palette expander
inherited from the OpenLara port emits red in the **high** bits,

```c
// display_esplcd.c:134
uint16_t rgb565 = (r << 11) | (g << 6) | b;   // 5-6-5, G gets the extra bit
```

from an identically-laid-out BGR555 source, and that port runs on the same board with
the same `board_pins.h`. Both cannot be right for one MADCTL setting. Either the 555
path shows red and blue swapped on the panel, or the palette path does. It has not
been checked against the physical panel with a reference image, and the serial screen
dump (§4.5) reconstructs colour from VRAM rather than from what the panel received,
so it cannot detect the difference.

The fix is one token in either place. It is also worth noting that the trick no longer
buys anything measurable: the present runs on core 1 and is transfer-bound (15.4 ms of
DMA against ~3 ms of conversion for a whole frame), so two extra shifts per pixel
would disappear into the DMA wait. This is on the list.

### 3.4 Runtime rescale: one panel, two video modes

The PlayStation changes video mode at runtime, and Driver 2 uses that: the frontend
draws **640×512** hi-res interlaced (`FEmain.c:1785-1788`) and the game itself draws
**320×240** (`system.c:729-730`). The first version of the present simply cropped,
which meant the frontend showed its top-left quarter.

The fix is a 16.16 nearest-neighbour walk that maps whatever the display environment
asks for onto the fixed panel:

```c
// display_esplcd.c:275-302
void displayFlush555Scaled(const uint16_t* src, int stride, int sw, int sh)
{
    // 16.16 nearest-neighbour steps, so any PSX video mode fills the panel.
    const uint32_t xStep = ((uint32_t)sw << 16) / DISPLAY_WIDTH;
    const uint32_t yStep = ((uint32_t)sh << 16) / DISPLAY_HEIGHT;
    ...
        const uint16_t* s = src + (((uint32_t)(y + l) * yStep) >> 16) * stride;
        uint32_t sx = 0;
        for (int x = 0; x < DISPLAY_WIDTH; x++, sx += xStep) {
            uint16_t c = s[sx >> 16];
            ...
        }
}
```

At 320×240 both steps are exactly `0x10000` and the loop degenerates to a copy — the
game path pays one add and one shift per pixel for the ability to also display the
frontend. At 640×512 the steps are 2.0 and 2.133, so the frontend is point-sampled
down. Point sampling a hi-res 2D menu is visibly harsh, but it is honest to the source
pixels and costs nothing; a box filter would double the reads on the path that is
already the frame's longest transfer.

The source rectangle comes from the display environment and is clamped so a bad
`DISPENV` can never read past VRAM:

```c
// esp_platform.cpp:94-100
const int dx = activeDispEnv.disp.x & (VRAM_WIDTH - 1);
const int dy = activeDispEnv.disp.y & (VRAM_HEIGHT - 1);
int dw = activeDispEnv.disp.w, dh = activeDispEnv.disp.h;
if (dw <= 0 || dh <= 0) return 1;
if (dw > VRAM_WIDTH - dx)  dw = VRAM_WIDTH - dx;    // never read past VRAM
if (dh > VRAM_HEIGHT - dy) dh = VRAM_HEIGHT - dy;
```

### 3.5 Asynchronous present on core 1, and how double buffering is detected

Profiling showed the present costing 17 ms per frame. A 320×240×2-byte frame at 80 MHz
is 15.36 ms of pure transfer, so ~90% of that was the game task blocked on the
DMA-complete semaphore, doing nothing. Moving it to core 1 removes essentially the
whole cost from the frame's critical path.

The worker is a notification-driven task pinned to core 1:

```c
// display_esplcd.c:228-252
static void presentTask(void* arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        displayFlush555Scaled(sPresentJob.src, sPresentJob.stride,
                              sPresentJob.sw, sPresentJob.sh);
        xSemaphoreGive(sPresentIdle);
    }
}
```

`displayPresentAsync` takes `sPresentIdle` *before* publishing the job
(`display_esplcd.c:267-272`), so at most one flush is ever in flight and the caller
blocks only if the previous frame's transfer has not finished — which, at 16.8 fps
against a 15.4 ms transfer, it always has. If the worker failed to start, the function
falls back to an inline flush (`display_esplcd.c:263-266`), so the feature is
strictly additive.

**Why it is safe.** The source buffer is still being transmitted while the game draws
the next frame. That is only acceptable if the next frame goes somewhere else — i.e.
if the PSX is double buffering. Rather than assume it, the port detects it from the
data:

```c
// esp_platform.cpp:136-144
// Detect that from the display origin alternating between frames: in game
// it toggles 0 / 256 every frame, while the 640x512 frontend keeps both
// buffers at the same place and must therefore flush inline.
static int lastDy = -1;
const bool doubleBuffered = (lastDy >= 0 && lastDy != dy);
lastDy = dy;
...
if (doubleBuffered)
    displayPresentAsync(&vram[dy * VRAM_WIDTH + dx], VRAM_WIDTH, dw, dh);
else
    displayFlush555Scaled(&vram[dy * VRAM_WIDTH + dx], VRAM_WIDTH, dw, dh);
```

This is directly readable in the game's own setup. In game, `system.c:729-730` places
the two display environments at `y = 256` and `y = 0`, so `dy` alternates every frame
and the test passes. In the frontend, `FEmain.c:1785-1788` gives **both** buffers the
same `draw_mode.y1`, so `dy` never changes, the test fails, and the frontend flushes
inline — where it cannot tear because the game task is blocked for the duration.

The heuristic has one soft edge, stated for completeness: on the single frame where
the mode changes (frontend → game), `dy` changes once for a non-double-buffered
reason, so that one frame is presented asynchronously on a possibly-shared buffer.
The worst case is a single torn frame during a screen transition that is already
covered by a loading screen. Making it exact would mean tracking both `DISPENV`s
rather than the last origin.

The measured outcome, from the 60-frame profile line printed in
`PsyX_EndScene` (`esp_host.cpp:96-108`):

| Stage | Cost at 16.8 fps |
|---|---:|
| Rasterization (slower of the two core bands) | 32 ms |
| Game logic + software GTE | 27 ms |
| Panel present (as seen by the game task) | 0 ms |

---

## 4. Input

### 4.1 Physical buttons

Ten GPIO lines on header P1, configured as inputs with internal pull-ups in one
`gpio_config` call (`esp_platform.cpp:222-232`), plus GPIO0 (the BOOT button) as an
extra CROSS. Two of the ten (16 and 21) already have 4.7K on-board pull-ups from the
camera SCCB circuit; the rest rely on the internal ones. The mapping table is flat and
data-driven:

```c
// esp_platform.cpp:189-200
static const struct { uint8_t gpio; uint16_t mask; } sButtons[] = {
    { PIN_BTN_UP,     PAD_UP },
    { PIN_BTN_DOWN,   PAD_DOWN },
    ...
};
```

### 4.2 Boot-time detection of lines this board holds low

This one cost real debugging time and is the kind of thing that only shows up on
hardware. GPIO17 on the Waveshare S3-Touch-LCD-2 reads 0 at reset even with the
internal pull-up enabled — the board drives it. With the line enabled as a button, the
pad reported CROSS as permanently pressed. Because the frontend navigates on
*new* presses (`Pads[pad].dirnew = buttons & ~mapped`, `pad.c:182`), a permanently held
button never produces an edge, and the menu was simply dead.

The port does not hardcode the exception; it measures once at init:

```c
// esp_platform.cpp:234-245
// A line that is already low at reset is not a pressed button — it is a pin
// this board drives itself (GPIO17 on the Waveshare S3-Touch-LCD-2 has an
// external pull-down that beats the internal pull-up). Left enabled it
// reads as a permanently held button, which kills edge-triggered menu
// input. Sample once and drop those lines.
for (unsigned i = 0; i < sizeof(sButtons)/sizeof(sButtons[0]); i++) {
    const int level = gpio_get_level((gpio_num_t)sButtons[i].gpio);
    if (!level) {
        sButtonStuck |= 1u << i;
        ESP_LOGW(TAG, "button gpio%d stuck low at boot - disabled", sButtons[i].gpio);
    }
}
```

The disabled set is then skipped in the poll (`esp_platform.cpp:337-339`). This
generalizes: plug the port into a different carrier board and it will disable whatever
that board happens to drive, and say so in the log. The trade-off is that a button
genuinely held down during reset is disabled for the session — acceptable, and
detectable from the warning line.

### 4.3 The PSX pad bit layout

The masks are defined once, in the order of the 16-bit word the game assembles:

```c
// esp_platform.cpp:172-187
// PSX pad bits, active low, in the word the game builds as
// (padRaw->buttons[0] << 8) | padRaw->buttons[1]  (see MapPad in pad.c).
// buttons[0] carries d-pad/start/select, buttons[1] the face and shoulder keys.
```

| Bit | Button | Byte |
|---:|---|---|
| `0x0001` | L2 | `buttons[1]` |
| `0x0002` | R2 | `buttons[1]` |
| `0x0004` | L1 | `buttons[1]` |
| `0x0008` | R1 | `buttons[1]` |
| `0x0010` | TRIANGLE | `buttons[1]` |
| `0x0020` | CIRCLE | `buttons[1]` |
| `0x0040` | CROSS | `buttons[1]` |
| `0x0080` | SQUARE | `buttons[1]` |
| `0x0100` | SELECT | `buttons[0]` |
| `0x0800` | START | `buttons[0]` |
| `0x1000` | UP | `buttons[0]` |
| `0x2000` | RIGHT | `buttons[0]` |
| `0x4000` | DOWN | `buttons[0]` |
| `0x8000` | LEFT | `buttons[0]` |

An early version had the two bytes swapped relative to this, which produced a pad that
responded to everything except what you pressed. The layout is the real hardware's:
`buttons[0]` is the first byte off the controller (SELECT, L3, R3, START, then the
d-pad), `buttons[1]` the second (shoulder buttons then the four face buttons).

### 4.4 Publishing through PADRAW, not `PadGetState`

This is the non-obvious part. PsyCross has a pad API (`PsyX_Pad_GetStatus`), and
implementing it is the natural thing to do — but nothing filled the pad in the first
boot attempt anyway. Driver 2 does not call it. `InitControllers` registers two raw
report buffers with `PadInitDirect`:

```c
// pad.c:54
PadInitDirect((unsigned char*)padbuffer[0], (unsigned char*)padbuffer[1]);
```

and then reads those buffers directly every frame, decoding them in `MapPad`:

```c
// pad.c:167-181
if (pData->id >> 4 == 4)      Pads[pad].type = 2;
else if (pData->id >> 4 == 7) Pads[pad].type = 4;
else { Pads[pad].type = 1; return; }
...
buttons = ~((pData->buttons[0] << 8) | (pData->buttons[1])) & 0xffff;
```

So the function that actually has to publish state is `PsyX_UpdateInput`, and it has
to write a well-formed PSX controller report — status, id, and both button bytes:

```c
// esp_host.cpp:120-136
// Driver 2 does not go through PadGetState — ReadControllers() reads the raw
// PADRAW buffers registered by PadInitDirect. So this, not PsyX_Pad_GetStatus,
// is what actually has to publish the pad state each frame.
void PsyX_UpdateInput(void)
{
    const uint16_t buttons = esp_input_poll();   // active low

    if (sPadData[0]) {
        u_char* p = sPadData[0];
        p[0] = 0x00;                        // status: ok
        p[1] = 0x41;                        // id: digital pad, 1 halfword
        p[2] = (u_char)(buttons >> 8);      // MapPad reads buttons[0] as the high byte
        p[3] = (u_char)(buttons & 0xFF);
    }
    if (sPadData[1])
        sPadData[1][0] = 0xFF;              // slot 2 disconnected
}
```

`0x41` is not arbitrary: `pad.c:432` accepts only ids 65 (`0x41`) or 115, and
`MapPad` derives the controller type from `id >> 4`, so `0x41` means "digital pad, one
halfword of data" and yields `type = 2`. The buffers themselves are captured in
`PsyX_Pad_InitPad` (`esp_host.cpp:176-179`), which is PsyCross's hook for
`PadInitDirect`.

The port keeps its mask **active low** all the way through, exactly as the hardware
would deliver it, and lets `MapPad`'s existing `~` do the inversion. No polarity
translation layer, nothing to get backwards.

### 4.5 The host pad protocol

A serial console transmits key *presses*. It cannot express hold-and-release, which is
fine for a menu and useless for a throttle or a steering wheel. So the firmware accepts
two input styles at once.

**Taps** — single characters, mapped in `serialMask` (`esp_platform.cpp:203-220`):
`wasd` for the d-pad, `x`/`z`/`q`/`e` for cross/circle/square/triangle, Enter for
START, space for SELECT, `1`/`2` for L1/R1. Each sets a countdown so the button is held
for `SERIAL_HOLD = 6` frames (`esp_platform.cpp:317, 324-326`), long enough for the
game to register an edge.

**The full pad word** — `"=HHHH"`, four hex digits, **active high**, sent as often as
the host likes:

```c
// esp_platform.cpp:288-305
// mid-way through a "=HHHH" packet: consume digits, never keys
if (sHexCount >= 0) {
    const int v = hexVal(buf[i]);
    if (v < 0) { sHexCount = -1; }              // malformed, resync
    else {
        sHexAcc = (uint16_t)((sHexAcc << 4) | v);
        if (++sHexCount == 4) {
            sHostMask = sHexAcc;
            sHostMaskMs = (uint32_t)(esp_timer_get_time() / 1000);
            sHexCount = -1;
        }
    }
    continue;
}
if (buf[i] == '=') { sHexCount = 0; sHexAcc = 0; continue; }
```

Three properties make this robust over an unreliable console link:

* **Stateless.** Each packet carries the complete button state, so a dropped or
  corrupted byte self-corrects on the next update 20 ms later.
* **Times out.** The latch is only honoured for `HOST_MASK_TIMEOUT_MS = 400`
  (`esp_platform.cpp:329-335`). A host that crashes or disconnects releases everything
  rather than leaving the throttle pinned.
* **Cannot collide with taps.** `=` is not a tap key, and while parsing digits the
  loop `continue`s past the tap dispatch, so `=00c0` can never be read as the letters
  `c` and `0`.

The three sources are merged in `esp_input_poll`: taps clear their bits, the host latch
clears `sHostMask`, and GPIO reads clear theirs — all into one active-low word
(`esp_platform.cpp:323-341`).

One bug had to be fixed for this to work at all. The original poll drained a fixed 16
bytes per frame. At 50 Hz a 5-byte packet is 250 bytes/s against a 16-byte-per-frame
drain; the receive buffer backed up until the host's `Write` blocked and timed out. The
loop now drains to empty:

```c
// esp_platform.cpp:281-285
// Drain the whole receive buffer, not a fixed nibble of it: the host sends
// a 5-byte pad packet 50 times a second, which outruns any per-frame cap
// and eventually backs up until the host's write blocks.
uint8_t buf[64];
int n;
while ((n = usb_serial_jtag_read_bytes(buf, sizeof(buf), 0)) > 0)
```

The host side is `esp32_port/drive.ps1`. It reads true key state (down *and* up)
through `GetAsyncKeyState`, builds the same 16-bit word from the same bit table
(`drive.ps1:32-57`), and writes `"=" + $mask.ToString("x4")` every 20 ms
(`drive.ps1:102-108`). Two details make it well-behaved: it only samples the keyboard
while its own console window has focus (`Kbd::Focused()`, `drive.ps1:85`), so
alt-tabbing releases the pad; and its `finally` block writes `"=0000"` before closing
the port (`drive.ps1:111`), so quitting does not leave a car driving into a wall.

Controls: arrows or WASD steer and navigate, X accelerates and confirms, Z brakes,
Space is the handbrake, C is wheelspin, Shift is fast steer, Enter/Backspace are
START/SELECT.

**Debug keys.** The same input path carries single-shot diagnostics, which is how the
renderer was debugged without a JTAG probe: `p` dumps the display rectangle as a
160×120 RGB332 hex image, `v` the whole VRAM at half scale, `r` raw VRAM words of one
texture page, `t` a texture page decoded exactly as the rasterizer samples it, and `f`
toggles the minification filter live so the two can be compared on the panel without
reflashing (`esp_platform.cpp:306-314`, `dumpScreen` at `esp_platform.cpp:63-86`).
`drive.ps1` fires the first four on key-down edges only (`drive.ps1:60, 92-96`).

---

## 5. Flash layout

`esp32_port/partitions.csv`:

| Name | Type | SubType | Offset | Size | |
|---|---|---|---:|---:|---|
| `nvs` | data | nvs | `0x9000` | `0x6000` | 24 KB |
| `phy_init` | data | phy | `0xf000` | `0x1000` | 4 KB |
| `factory` | app | factory | `0x10000` | `0x400000` | 4 MB |
| `gamedata` | data | `0x40` | `0x410000` | `0xBF0000` | 11.94 MB |

```
# demo data (OLVL container): Havana level + frontend assets + sound banks.
# The sound banks must ship even before audio exists — the frontend blocks on
# a failed VOICES2.BLK open. Sized to the end of the 16MB flash.
```

`0x410000 + 0xBF0000 = 0x1000000` — the data partition runs to the last byte of the
16 MB flash. It has to: the demo image is 10.96 MB with sound. The 4 MB app partition
is enormous by comparison (the firmware is 674 KB) but the alignment is convenient and
there is nowhere else for the slack to go.

The subtype `0x40` is a private value; `esp_fs_mount` looks the partition up by that
subtype *and* the name `gamedata` (`esp_fs.c:222-223`).

Two entries in that comment are lessons, not decisions. `SOUND/VOICES2.BLK` and
`SOUND/MUSIC.BIN` were originally excluded to save 3.2 MB — there is no audio on this
target yet, so why ship banks? Because the boot sequence blocks on the open failing.
Same for `GFX/SPLASH1N.TIM`. Data that the game merely *touches* is as mandatory as
data it uses.

---

## 6. Boot order

`app_main` is short enough to read whole, and the order matters:

```c
// esp_main.cpp:79-122
g_softRasterEnabled = 1;   // no OpenGL here — the rasterizer owns the frame
gDataFolder[0] = 0;        // the flash container stores paths relative to the data root
if (!allocPsxMemory())   { ... return; }   // 2.9 MB of PSRAM buffers
if (esp_fs_mount() != 0) { ... return; }   // mmap + VFS at /d
if (displayInit() != 0)  { ... return; }   // SPI bus, ST7789, black frame, backlight on
displayPresentAsyncInit();                 // present worker on core 1
SoftRas_ForkInit();                        // rasterizer worker on core 1
esp_input_init();                          // GPIO + USB-serial-JTAG driver
... xTaskCreateStaticPinnedToCore(gameTask, "game", 96 KB PSRAM stack, ..., core 0);
```

The memory map is allocated before anything else so a failure is reported as a clean
"PSX memory map allocation failed" rather than a null dereference three subsystems
later. The display is brought up with the backlight **off**, pushes one black frame,
and only then enables the backlight (`display_esplcd.c:55-60, 116-123`), so panel
initialization garbage is never visible. Every step logs and returns on failure rather
than aborting, so a board with unflashed game data prints "flash gamedata.bin first"
and sits there instead of boot-looping.

---

## 7. Open items and trade-offs

| Item | Status |
|---|---|
| Red/blue channel order (§3.3) | The 555 present path and the 8bpp palette path in `display_esplcd.c` disagree, and `board_pins.h` declares RGB element order, which clears the MADCTL BGR bit the 555 path's comment relies on. Needs a check against the physical panel; the fix is one token. |
| Config / progress persistence (§1.7) | `getenv("HOME")` is `NULL`, so the profile path becomes absolute `/config.dat` and misses the `/d` mount entirely. Saves are silent no-ops. |
| RAM files are session-only | No NVS write-back yet. |
| Frontend rescale is point-sampled | 640×512 → 320×240 nearest neighbour. Harsh on hi-res 2D, but the present is already the frame's longest transfer. |
| Double-buffer detection (§3.5) | One frame at a mode change can be presented asynchronously on a heuristic that is momentarily wrong; worst case is a torn transition frame. |
| Audio | Every `PsyX_SPUAL_*` and XA entry point is a deliberate no-op returning "idle" so the mixer-driven game logic keeps advancing (`esp_host.cpp:210-226, 236-245`). I2S is a later phase. |
| Touch and IMU | Wired and in the pin map, unused. |
| microSD | On the same SPI2 bus; `PIN_SD_CS` is driven high at init (`display_esplcd.c:56-62`) so the card never drives MISO. No SDSPI driver is instantiated. |
