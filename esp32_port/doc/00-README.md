# Driver 2 on an ESP32-S3 — documentation index

Driver 2 (PlayStation 1, 1999) runs natively on a Waveshare ESP32-S3-Touch-LCD-2:
a 240 MHz dual-core Xtensa LX7 with 512 KB of internal SRAM, 8 MB of octal PSRAM
and a 320x240 ST7789 panel on SPI. It boots to the frontend, spools the Havana
demo city out of memory-mapped flash, and is playable.

It is **not** an emulator. There is no MIPS interpreter and no PSX BIOS. The base
is [REDRIVER2](https://github.com/OpenDriver2/REDRIVER2), a complete
decompilation of the retail game into C, cross-compiled for Xtensa. The PSX SDK
underneath it is PsyCross ("PsyX"), whose GPU backend normally translates the PSX
primitive stream into OpenGL. That backend is the part that could not come along,
so the port is two pieces of new work:

1. **A software rasterizer** for the PSX primitive stream
   (`src_rebuild/PsyCross/src/gpu/PsyX_SoftRas.cpp`, 1117 lines), consuming the
   same ordering-table packets the real GPU would and writing 15-bit BGR555
   pixels into the emulated 1 MB VRAM array — with PSX semantics: affine
   interpolation, CLUT lookup, the texture-window formula, the four blend modes
   with per-texel STP bit, and painter ordering with no depth buffer.
2. **An ESP32 platform layer** (`esp32_port/`, an ESP-IDF v5.5 project) supplying
   everything the SDL/OpenGL host used to: memory map, filesystem, pad, timing,
   present, logging.

These eleven documents are the engineering record of that work: the architecture
and where it cuts into the codebase, the rasterizer and its correctness defects,
the platform layer, the bring-up, the performance methodology, the dual-core
split, the texture filter, the debugging toolkit, and how to build and run it.

---

## Headline status

| | |
|---|---|
| **In-game frame rate** | **16.8 fps**, 59 ms/frame, unfiltered |
| First frame that rendered at all | 200 ms, 5.0 fps |
| Rasterizer inner loop | **221 → 68 cycles per covered pixel** |
| With the optional minification filter on | 5-9 fps while driving. Its cost was measured as a same-scene A/B, **14.5 → 10.0 fps** — 14.5 is that scene's baseline, not the 16.8 fps headline |
| Dual-core correctness | byte-identical to single core: **0 of 19200 sampled pixels differ** |
| Firmware / data | 674 KB app, 10.95 MB `gamedata` image — 11,487,176 bytes (free DRIVER2DEMO set) |

Where the 59 ms goes:

| Stage | Time | Note |
|---|---:|---|
| Rasterisation | 32 ms | the slower of the two core bands; the other is 29 ms |
| Game logic + software GTE + OT walk | 27 ms | entirely serial, entirely on core 0 |
| Panel present | 0 ms | 15.4 ms of SPI DMA, moved to a task on core 1 |

Two of those moves are worth keeping straight, because they are easy to
conflate. The rasterizer rework ended at **89 ms / 11.1 fps**. Moving the panel
present off the game task to core 1 is what took the frame to **74 ms /
13.5 fps** — 47 ms of rasterisation plus 27 ms of everything else. Splitting the
rasterisation itself across both cores then took it to **59 ms / 16.8 fps**,
because the frame waits for the slower band: 32 + 27.

Three measurements shape everything else, and all three were taken on the device:

- Texture fetch costs **~34 cycles/pixel** — half the inner loop (measured by
  stubbing the fetch out).
- Framebuffer writes to PSRAM are **essentially free**: redirecting them to
  internal SRAM changed 96 → 95 cycles/pixel. The loop is ALU-bound, not
  memory-bound, and a band-rendering redesign was rejected on that evidence.
- The frame's two texel-per-pixel populations: ground/buildings/foliage at
  1.2-1.6 on 100-200 pixel polygons, the player car at 11-23 on 2-3 pixel
  polygons.

**Not working:** audio (every `PsyX_SPUAL_*` entry point is a no-op), retail
cities (only the free demo set fits in 16 MB of flash), attract demos/replays,
FMV and XA speech, persistent saves, touch, IMU and microSD.

---

## The documents

| # | Document | What it covers |
|---|---|---|
| 01 | [What was built](01-what-was-built.md) | Orientation: what the claim does and does not mean, what runs today, where the time goes, repository tour, the list of upstream files that were modified. |
| 02 | [Where the port cuts in](02-where-the-port-cuts-in.md) | Architecture. The seam at `ParsePrimitive`, why that layer and not one above or below, what became dead code, the compile-time configuration (`PSYX_NO_RENDERER`, `USE_PGXP=0`) and the force-included `esp_compat.h`. |
| 03 | [The software rasterizer](03-the-software-rasterizer.md) | The pixel engine in data-flow order: `SRState`, PSX tpage/CLUT encodings, texel fetch, the triangle rasterizer, the span solver, affine 16.16 interpolation, the pixel kernels, blending, the minification kernel, primitive dispatch, and known divergences from the GL backend. |
| 04 | [Rasterizer correctness](04-rasterizer-correctness.md) | Nine correctness defects: the symptom on a 320x240 panel, how each was cornered without a debugger, and the actual repair. Includes the offline audit programs and the pixel-identical `HAVANA.LEV` comparison. |
| 05 | [The ESP32 platform layer](05-the-esp32-platform-layer.md) | Everything in `esp32_port/main/`: the OLVL flash container and VFS at `/d`, memory placement and the PSRAM budget, the ST7789 pipeline (chunked DMA, 555→565, rescale, async present), input, flash layout, boot order. |
| 06 | [Bring-up log](06-bringup-log.md) | The chronological account of eight failures between "the firmware links" and "you can drive it", each one hiding the next, plus the four instruments that found them and the `addr2line` workflow. |
| 07 | [Performance](07-performance.md) | The measurement methodology first, then each optimisation step with its number — and the section that took the most work to earn: the two redesigns the measurements *killed*. |
| 08 | [Dual-core rasterisation](08-dual-core-rasterisation.md) | How two cores walk one sequential ordering table without a lock in the pixel path: the band split, the three pieces of state that had to become per-core, the barrier for out-of-band side effects, the load balancer, the safety net, and the byte-identical verification. |
| 09 | [Texture filtering](09-texture-filtering.md) | Why the surfaces looked like static, why the first hypothesis (corrupt texture data) was wrong, the adaptive 2x2/4x4 rotated-grid kernel, what it costs, and why mip levels are blocked for 4bpp CLUT textures. |
| 10 | [Debugging toolkit](10-debugging-toolkit.md) | Replacing a debugger with a serial console: the `p`/`v`/`t`/`r` dumps and the host-side PNG decoder, the frame profiler, compile-time ablation as a method, `SoftRas_DebugDecodeTPage`, panic decoding, and an independent Python decoder for `HAVANA.LEV`. |
| 11 | [Building and running](11-building-and-running.md) | Prerequisites, `build.ps1` and why not `idf.py`, producing and flashing the game-data image, the partition layout, reading the serial log, the controls, and troubleshooting. |

### Reading order — to understand the port

Roughly two hours, in this order:

1. **01** — what the claim means, what works, what does not.
2. **02** — the one architectural decision everything else follows from: where the
   seam was cut and why.
3. **03**, sections 1-6 — how a PSX primitive becomes pixels.
4. **07** — where the time actually goes, and how that was established.
5. **08** — the most interesting piece of engineering in the port.
6. **04** and **09** — pick either; both are case studies rather than reference
   material, and both are readable on their own.

Skip **05**, **10** and **11** unless you want the host layer, the tooling or the
operational detail.

### Reading order — to work on it

1. **11** — get it building, flashed and running first. Nothing else is checkable
   until then.
2. **10** — the tools you will use to see anything at all. In particular the
   dumps, the profile line, and the ablation switches.
3. **02** — where your change belongs in the layering, and what is deliberately
   dead code.
4. **05** — the memory budget and the ESP32-specific traps (PSRAM `.bss`, ISRs on
   task stacks, DMA-capable buffers).
5. **03**, section 10, and **08**, section 9 — the two lists of open items,
   divergences and latent traps. Read them before proposing anything.
6. **07**, section 3, and its "next targets" list — the measurements that already
   ruled out two plausible redesigns, and where the remaining headroom is.
7. **04** and **06** — the failure catalogue, for when your change breaks
   something in a way that looks familiar.

---

## Start here: three results worth the detour

Even if you read nothing else, these three sections carry the most transferable
content.

### 1. The undefined-behaviour float cast — `04-rasterizer-correctness.md`, Defect 1

The row-start texture coordinate was computed by casting a `float` to `int`. The
start point is a corner of the *clipped* bounding box, which for a sliver
triangle sits far off the triangle, so the extrapolated 16.16 value exceeded
2³¹ — and an out-of-range float→int conversion is undefined behaviour that in
practice saturates to `INT_MIN`. That poisoned every row of the primitive with a
constant garbage offset. Thin geometry was wrong, fat geometry was fine.

The fix computes the same quantity in `unsigned` arithmetic, and the argument for
why that is *correct* rather than merely tolerable is the payload: unsigned
overflow is exact modulo 2³², the per-pixel accumulator wraps in the same ring,
and the wrap cancels exactly at every covered pixel. See also
`03-the-software-rasterizer.md` §5.5 for the same code from the design side.

> `audit2.cpp` output: `triangles with >1 texel u error: 217 / attributable to
> uRow (int) cast overflow : 217 / UNEXPLAINED (pure precision) : 0`

### 2. The dual-core band split, verified byte-identical — `08-dual-core-rasterisation.md`

A PSX ordering table is a sequential structure: Driver 2 has no depth buffer, so
the image is correct only if primitives land in list order. Both cores are
nonetheless handed the *same* list and walk it concurrently, each clamped to a
horizontal band of VRAM. Every pixel is written by exactly one core, so painter
ordering holds pixel by pixel and there is no lock anywhere in the pixel path.
13.5 → 16.8 fps.

Read §1 (why ordering survives), §3 (the three pieces of state that had to become
per-core), §4 (the two primitives per frame that need a rendezvous, and why the
notification counts provably pair up), and §7 (the verification: 0 of 19200
sampled pixels differ). §5 and §9 are honest about what the balancer does *not*
currently achieve.

### 3. The minification-aliasing diagnosis — `09-texture-filtering.md` §1-2

The surfaces rendered as statistical noise, and the obvious suspect — texture
data corrupted somewhere along a completely rewritten path from flash to VRAM —
was wrong. §1 is the bisection that proved it: raw VRAM words pulled off the
board over serial and compared against the same page decoded from `HAVANA.LEV` on
the host, pixel-identical.

What was left is not a bug at all. The PSX takes exactly one texel per pixel;
when geometry recedes, that single sample is a uniform random draw from a
footprint many texels across. A CRT low-passed the result away and a sharp LCD
does not. The fix is an adaptive rotated-grid kernel whose sample pattern is
built from the UV Jacobian the rasterizer already carries, so it is anisotropic
for free — and whose 4x4 form is affordable only because of the measured split
between the two texel-per-pixel populations.

---

## Glossary

Terms used throughout, in the sense this project uses them.

| Term | Meaning |
|---|---|
| **OT** (ordering table) | The PSX display list: an array of buckets, each the head of a linked list of primitive packets. The game adds primitives with `addPrim` and hands the whole table to `DrawOTag`. It is walked in reverse depth order, so it *is* the painter's algorithm — there is no depth buffer anywhere in Driver 2. |
| **tpage** (texture page) | A 16-bit word selecting a 256x256-texel region of VRAM as the current texture source, plus the colour format (4bpp / 8bpp / 15bpp direct) and the semi-transparency blend rate. Set by a `DR_TPAGE` packet or carried in a textured polygon; sprites and tiles have no tpage field of their own and use whatever the draw environment currently holds. |
| **CLUT** (colour lookup table) | The palette for an indexed texture, stored *inside* VRAM as a row of 16 (4bpp) or 256 (8bpp) 15-bit entries. A CLUT word encodes its VRAM x/y. Entry value `0x0000` means fully transparent. The rasterizer copies the 16-entry form into its per-primitive state once, because reading it per pixel costs a cache miss. |
| **DRAWENV** | The GPU's drawing state: clip (scissor) rectangle, draw offset, current tpage, texture window, dither and draw-to-display flags. It is not static input — `DR_TPAGE` / `DR_TWIN` / `DR_AREA` / `DR_OFFSET` packets embedded in the ordering table mutate it as the walk proceeds, which is why it had to become `g_drawEnv[2]`, one per core. |
| **PSRAM** | The ESP32-S3's external pseudo-static RAM: 8 MB, octal, 80 MHz, reached through the 64 KB data cache. Large and cold data lives here — the 1 MB emulated VRAM, the 2 MB PSX RAM arena, the game's big buffers. It is not usable for DMA on the panel path, and a task whose stack is in PSRAM cannot safely be interrupted while the cache is disabled. |
| **IRAM** | Internal SRAM mapped as instruction memory. Code that must run with the flash cache disabled (ISRs, notably the panel's DMA-completion handler) has to be IRAM-resident. Internal SRAM is the scarce resource here: about 20 KB is free once the application is up. |
| **VFS** | ESP-IDF's virtual filesystem layer. The port registers eight callbacks under the mount point `/d`, backed by the memory-mapped `OLVL` flash container, so the game's unmodified `fopen`/`fread` calls resolve against flash with zero RAM cost. |
| **span** | The run of pixels on one scanline that a triangle actually covers. The rasterizer solves the three edge half-planes once per row to get `[xs, xe]` directly, instead of testing coverage per pixel — the single largest inner-loop win. "Covered pixels" and "span pixels" mean the same thing, and are the denominator of every cycles-per-pixel figure in these documents. |
| **texel** | One sample of a texture, as opposed to a pixel on screen. "Texels per pixel" is the compression ratio of a surface: above 1 the pixel's footprint spans several texels and a single sample aliases. |
| **STP bit** | Bit 15 of a 15bpp PSX pixel (`r \| g<<5 \| b<<10 \| stp<<15`): the per-texel semi-transparency flag. A texel is blended only if *both* the primitive's `abe` bit and the texel's own STP bit are set. It is not a colour channel, which is why an averaging filter that rebuilds a pixel from its three channels silently drops it — and turns the car's shadow into an opaque black rectangle. |

Two more that appear constantly and are easy to conflate:

- **Fully transparent** (`0x0000`) means the texel is *not written at all*.
  **Semi-transparent** (STP bit set) means it *is* written, through one of the
  four blend modes. They are tracked separately: transparency through the
  `clutNZ` bitmask built from the raw palette, semi-transparency through bit 15
  of the pixel.
- **`P_LEN`** is the primitive header size in words (2 on this 32-bit target).
  Every primitive handler returns its payload length *excluding* the header, and
  that return value is the stride of the cursor that walks concatenated
  primitives inside one OT tag — get it wrong by one word and the rest of the tag
  decodes as garbage.

---

The git history of both repositories is the primary record; every commit message
explains the reasoning, not just the change:

```
git -C E:/Hardware/Driver2/REDRIVER2 log --format=%B
git -C E:/Hardware/Driver2/REDRIVER2/src_rebuild/PsyCross log --format=%B
```
