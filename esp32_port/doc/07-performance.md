# Performance: 5 to 16.8 fps

The first frame Driver 2 ever drew on the ESP32-S3 took 200 ms. The same scene now
takes 59 ms. Nothing in that factor of 3.4 came from a better algorithm in the
abstract sense — the rasteriser is the same half-space triangle filler it was on
day one. It came from measuring the machine, four times, and each time removing
the thing the measurement pointed at.

This document is the methodology. It describes how the numbers were obtained
first, because the numbers are the only reason the changes are the changes; then
each step in order with its measurement; then, in the section that took the most
work to earn, the two redesigns that the measurements *killed*.

All figures are from the device: a Waveshare ESP32-S3-Touch-LCD-2, dual Xtensa
LX7 at 240 MHz, 8 MB octal PSRAM at 80 MHz, 64 KB data cache with 64-byte lines,
ST7789 320x240 panel on SPI at 80 MHz. The emulated 1 MB PSX VRAM lives in PSRAM
(`esp32_port/main/esp_host.cpp:254`).

---

## 1. How it was measured

### 1.1 There is no profiler

There is no debugger on this board and no sampling profiler in the build. What
exists is `esp_cpu_get_cycle_count()`, which on Xtensa reads the `CCOUNT` special
register, and a serial console. So the instrumentation is explicit: bracket the
regions of interest with cycle reads, accumulate into counters, and print a line
every 60 frames.

Three regions are bracketed, and between them they account for the whole frame:

| Region | Where | What it contains |
| --- | --- | --- |
| Rasterisation | `PsyX_GPU.cpp:1595-1601` | everything inside `SoftRas_Primitive()` — decode, state load, triangle setup, pixel loop |
| Present | `esp_platform.cpp:146-155` | the panel flush call in `SoftRas_PresentESP()` |
| Everything else | derived | game logic, the software GTE, the ordering-table walk, file I/O |

The rasteriser hook is the important one, because it is per *primitive* rather
than per frame — it isolates pixel work from the ordering-table walk that
surrounds it:

```c
// src_rebuild/PsyCross/src/gpu/PsyX_GPU.cpp:1593
if (g_softRasterEnabled)
{
#ifdef SOFTRAS_PROFILE
    extern unsigned g_dbgRasCycles[2];
    const unsigned c0 = esp_cpu_get_cycle_count();
    primLength = SoftRas_Primitive(polyTag);
    g_dbgRasCycles[xPortGetCoreID()] += esp_cpu_get_cycle_count() - c0;
#else
    primLength = SoftRas_Primitive(polyTag);
#endif
```

### 1.2 The counter is per-CPU, so the counters had to be per-core

`CCOUNT` is a per-core register. The two cores' counters run from independent
resets and are never synchronised, so a timestamp taken on core 0 and subtracted
from one taken on core 1 is meaningless — it is a difference of two unrelated
clocks. And a single shared accumulator incremented from both cores is a
non-atomic read-modify-write race on top of that.

Both problems disappear with the same change: index the accumulator by core.

```c
// esp32_port/main/esp_platform.cpp:54
// per core: the cycle counter is per-CPU, and the split also shows band balance
unsigned g_dbgRasCycles[2] = { 0, 0 };
```

This was not gold-plating for a hypothetical future. It became load-bearing the
moment rasterisation forked onto both cores (step 5), and the same array then
answered a second question for free: how evenly the two bands are loaded. The
frame waits for both cores, so the critical path is `max(c0, c1)`, not the sum —
and the *sum* divided by the pixel count is still the correct cost-per-pixel,
because both cores did real work. The report prints both.

### 1.3 The report

Every 60 frames, `PsyX_EndScene()` prints a three-line profile block
(`esp32_port/main/esp_host.cpp:82-118`) — the two below, plus the raster worker's
stack high-water figure (`esp_host.cpp:109`), which reads 4376 bytes free of the
6144 the task was created with:

```c
ESP_LOGI(TAG, "%d.%d fps  %dx%d  %lu prims/frame", ...);

// cycles -> ms at 240 MHz, averaged over the 60 frames. The frame
// waits for both cores, so the critical path is the slower band.
const unsigned long r0 = g_dbgRasCycles[0] / 240000 / 60;
const unsigned long r1 = g_dbgRasCycles[1] / 240000 / 60;
const unsigned long tot = g_dbgRasCycles[0] + g_dbgRasCycles[1];
ESP_LOGI(TAG, "  raster c0 %lu + c1 %lu ms/f  present %lu  other %lu | "
              "%lu tris/f  %lu kpx/f  %lu cyc/px  diverge %d", ...);
```

The fields, and what each is for:

| Field | Computed as | Why it is there |
| --- | --- | --- |
| `raster c0 + c1` | cycles / 240000 / 60 | ms per frame per core; the imbalance between them drives the band splitter |
| `present` | same | 0 once the flush moved to core 1 — and it self-reports if that ever stops being true (§2.5) |
| `other` | `frameMs - max(r0, r1)` | game logic + software GTE + OT walk, by subtraction |
| `tris/f`, `kpx/f` | counters / 60 | the denominators; a change that halves the pixel count is not the same as one that halves the cost per pixel |
| `cyc/px` | `(c0 + c1) / spanPixels` | **the single number the optimisation work was steered by** |
| `diverge` | canary | dual-core correctness (§2.6), not performance |

Wall-clock frame time comes from `esp_timer_get_time()` over the same 60 frames,
so the cycle-derived ms and the measured ms are independent — if they stop adding
up, something is being missed.

Two honest caveats about the counters:

- `g_dbgRasBboxPx` is a leftover name. Since the span solver landed
  (`PsyX_SoftRas.cpp:494-519`) it accumulates *span* pixels — pixels actually
  covered and written — not bounding-box pixels. `cyc/px` is therefore cost per
  covered pixel, which is what you want, but the identifier lies.
- `g_dbgRasBboxPx` and `g_dbgRasTris` are shared between the two cores and
  incremented non-atomically (`PsyX_SoftRas.cpp:399-402`, `:621-623`). Each
  increment is a handful of instructions once per triangle, against thousands of
  cycles of work per triangle, so lost increments are rare and always in one
  direction: pixel counts read slightly low, `cyc/px` slightly high. It does not
  change any conclusion below, but it is a real imprecision.

### 1.4 Ablation switches

Cycle counters tell you where time is spent. They do not tell you what would
happen if a component were free. Two compile-time switches answer that by
deleting a component and re-measuring — both deliberately render garbage, because
their only purpose is the number:

```c
// src_rebuild/PsyCross/src/gpu/PsyX_SoftRas.cpp:223
#ifdef SOFTRAS_NOTEX
    // [measurement build] no VRAM read at all: isolates texture-fetch cost
    // from framebuffer-write cost. Renders garbage on purpose.
    return (unsigned short)(0x1234 + u + v);
#endif
```

```c
// src_rebuild/PsyCross/src/gpu/PsyX_SoftRas.cpp:283
#ifdef SOFTRAS_FBSCRATCH
// [measurement build] one shared row in internal SRAM, used in place of the
// PSRAM framebuffer to isolate framebuffer-write cost. Renders nothing.
static unsigned short g_srScratchRow[1024];
#endif
```

`SOFTRAS_NOTEX` measured the texture fetch at ~34 cycles per pixel — half of the
final 68. `SOFTRAS_FBSCRATCH` produced the most consequential result in this
whole document, and it is a negative one (§3.1).

A third switch, `SOFTRAS_DEBUG_CAR` (`PsyX_SoftRas.cpp:404-425`), reports the
texel-per-pixel ratio of polygons landing where the player car sits. It is not a
timing tool; it characterises the *workload*, which is what later justified the
minification filter's shape.

### 1.5 Checking that the frame is still correct

Every change below is a semantics-preserving rewrite, and "semantics-preserving"
is a claim that has to be checkable without a camera. The serial console dumps
the panel contents as 160x120 RGB332 hex (`esp_platform.cpp:63-86`), whole VRAM at
half scale, the raw words of a texture page, and a texture page decoded exactly
the way the rasteriser samples it (`SoftRas_DebugDecodeTPage`,
`PsyX_SoftRas.cpp:755`). That last one exists specifically to separate "the
texture data is wrong" from "my sampling is wrong" — two failures that look
identical on a 2.8-inch panel.

---

## 2. What was tried, in order

Baseline and result:

| Step | Change | Frame | fps | Where |
| ---: | --- | ---: | ---: | --- |
| 0 | first frame that rendered at all | 200 ms | 5.0 | `47f4691e` / `bfbed95` |
| 1 | span solver, 32-bit edge functions, local render state | 109 ms | 9.2 | `4f049c6` |
| 2 | CLUT cached per primitive | 106 ms | 9.4 | `4f049c6` |
| 3 | pre-modulated CLUT, dead bounds masks, hoisted page pointer | 89 ms | 11.1 | `4f049c6` |
| 4 | panel present moved to core 1 | 74 ms | 13.5 | `fa95b9a6` |
| 5 | rasterisation on both cores | 59 ms | 16.8 | `d6dec58d` / `3750328` |

Steps 1-3 were separate measurement builds that landed as one commit; the fps
column for those is `1000 / frame` rather than a console reading. Steps 4 and 5
quote the fps the commits themselves report.

**The rasteriser rework ends at step 3: 89 ms, 11.1 fps.** 13.5 fps was not
earned by making the pixel loop faster — it was earned by moving the panel
present onto core 1 (`fa95b9a6`), which is a scheduling change and touches no
rasteriser code at all. Crediting 13.5 fps to the rasteriser work is a mistake
that is easy to make from the commit order and it inflates the apparent value of
the pixel-level optimisations by a third.

Two things to keep in mind when reading any of these numbers. fps depends on the
scene: a frame carries ~1200 primitives standing still against 2300-3100 while
driving (`0333687`). And raster time is itself scene dependent, so individual
console lines wander by a millisecond or two either side of the figures quoted
here; these are the representative values, not bounds. This is stated once, here,
and the rest of the documentation quotes the representative figure without
repeating the caveat.

Over steps 1-3 the rasteriser went from **221 to 68 cycles per covered pixel**.

### 2.1 Step 1a — solve the spans instead of testing the pixels

The original inner loop was the textbook half-space fill: walk the whole clipped
bounding box, and at each pixel test the three edge functions.

```c
// before
for (int y = minY; y <= maxY; y++)
{
    long long w0 = w0row, w1 = w1row, w2 = w2row;
    ...
    for (int x = minX; x <= maxX; x++)
    {
        if ((w0 | w1 | w2) >= 0)
        { ... }
        w0 += A12; w1 += A20; w2 += A01;
    }
}
```

Three 64-bit adds and a three-way sign test on every pixel of the bounding box,
including every pixel outside the triangle. In a 3D scene that overhead is not a
rounding error: geometry seen at a grazing angle — road strips, building facades,
the long thin quads Driver 2's city is built from — has bounding boxes several
times larger than its coverage.

But along a scanline `w` is *affine* in x: `w(x) = wRow + (x - minX) * A`. It is
monotone, so each edge contributes a half-line, and the intersection of three
half-lines is a single interval. Solve for it once per row and the coverage test
disappears from the pixel loop entirely:

```c
// src_rebuild/PsyCross/src/gpu/PsyX_SoftRas.cpp:502
#define SR_CLIP_EDGE(A, wrow)                                          \
    if ((A) > 0) {                                                     \
        if ((wrow) < 0) {                                              \
            const int d = (-(wrow) + (A) - 1) / (A);                   \
            if (minX + d > xs) xs = minX + d;                          \
        }                                                              \
    } else if ((A) < 0) {                                              \
        if ((wrow) < 0) { xs = xe + 1; }                               \
        else {                                                         \
            const int d = (wrow) / -(A);                               \
            if (minX + d < xe) xe = minX + d;                          \
        }                                                              \
    } else if ((wrow) < 0) { xs = xe + 1; }

SR_CLIP_EDGE(A12, w0row)
SR_CLIP_EDGE(A20, w1row)
SR_CLIP_EDGE(A01, w2row)
```

`A > 0` means `w` increases with x, so a negative row start becomes valid after a
ceiling-division number of steps — that is the `+A-1` rounding. `A < 0` gives an
upper bound by floor division. `A == 0` means the edge does not vary along the
row, so the row is either wholly in or wholly out. The three cases are exact
integer arithmetic; no pixel changes.

The attribute accumulators then have to be fast-forwarded to the span start
rather than the box start, which is one multiply per attribute per row instead of
one add per attribute per skipped pixel:

```c
// src_rebuild/PsyCross/src/gpu/PsyX_SoftRas.cpp:524
const int adv = xs - minX;
int u16 = uRow + duDx * adv, v16 = vRow + dvDx * adv;
```

The top-left fill rule is unaffected: the biases are folded into `w0row/w1row/w2row`
before the solve (`:335-341`), so the boundary decision is identical.

### 2.2 Step 1b — 32-bit edge functions

The edge functions and the winding area were `long long`. On Xtensa LX7 a 64-bit
multiply is not one instruction; a 64-bit add is two with carry handling. The
range argument shows the width was never needed:

```c
// src_rebuild/PsyCross/src/gpu/PsyX_SoftRas.cpp:275
// 32-bit is sufficient and much cheaper than 64-bit on Xtensa: PSX primitive
// coords are 11-bit signed and the draw offset adds at most 2047, so every
// term here stays under 2^25.
static inline int edgeFn(const SRVert& a, const SRVert& b, int px, int py)
{
    return (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
}
```

PSX vertex coordinates are 11-bit signed (-1024 … 1023) and `DRAWENV.ofs` adds at
most 2047, so a coordinate lives in -1024 … 3070 and any difference of two is
bounded by 4094. Each product is therefore at most 4094 x 4094 ≈ 1.7e7, and the
difference of two such products stays under 2^25 — which is exactly what the
comment claims. This is a property of the *hardware format being emulated*, not
an assumption about the content, which is what makes it safe rather than lucky.

### 2.3 Step 1c — pass the render state by reference from a caller-local copy

This is the first of the two instructive ones, because the code did not change
shape at all; only its aliasing did.

The primitive render state — texture page, CLUT coordinates, texture-window
masks, clip rectangle — lives in a file-scope object, `static SRState st`. The
pixel kernels read it. They also write pixels:

```c
row[x] = pix & 0x7FFF;      // row points into vram[]
```

`vram` is `unsigned short[]`. `SRState` contains `unsigned short clut16[16]` and
`const unsigned short* texPage` (`PsyX_SoftRas.cpp:74-76`). A store through an
`unsigned short*` may legally alias an `unsigned short` object, so the compiler
cannot prove that writing a pixel leaves the render state untouched — and it
took the only safe option available to it: reload every field it needed, on
every pixel. Field reloads of a struct that has not changed since the primitive
was decoded, inside the hottest loop in the program.

The fix is to break the dependency by construction. Copy the state into a local
whose address never escapes, and hand kernels a reference to *that*:

```c
// src_rebuild/PsyCross/src/gpu/PsyX_SoftRas.cpp:427
// Local copy of the render state. `vram` writes below would otherwise be
// assumed to alias the `st` global, forcing a reload of every field per
// pixel. Also hoist the framebuffer base for the same reason.
const SRState s = st;
unsigned short* const fb = vram;
```

and the fetch signature carries the reason with it, so nobody quietly reverts it:

```c
// src_rebuild/PsyCross/src/gpu/PsyX_SoftRas.cpp:216
// `s` is deliberately a reference to a CALLER-LOCAL copy of the render state,
// not the `st` global: the rasterizer writes through `vram`, and the compiler
// cannot prove those writes do not alias `st`, so reading the global here
// forces a reload of every field on every pixel.
template <int FMT>
static inline unsigned short fetchTexel(const SRState& s, int u, int v)
```

`s` is a `const` local, taken once per triangle, so the compiler is free to keep
its fields in registers for the whole span. The copy costs a few dozen cycles per
triangle; the reloads cost several per pixel.

The general lesson is worth stating plainly, because it generalises well beyond
this rasteriser: **on a target where you cannot afford redundant loads, a
file-scope variable read inside a loop that writes memory of a compatible type is
a per-iteration reload, not a constant.** The remedy is a local copy, and it is
free.

Steps 1a-1c together: **200 ms -> 109 ms.**

### 2.4 Step 2 — cache the CLUT per primitive (109 -> 106 ms)

Before this, a 4bpp texel fetch was *two* dependent PSRAM reads: one for the
4-bit index cell, then one into the palette row.

```c
// before
int x = st.texBaseX + (u >> 2);
unsigned short cell = vram[((st.texBaseY + v) & 511) * VRAM_WIDTH + (x & 1023)];
int idx = (cell >> ((u & 3) * 4)) & 0xF;
return vram[st.clutY * VRAM_WIDTH + ((st.clutX + idx) & 1023)];
```

The palette is 16 halfwords — half of one 64-byte cache line. It ought to stay
resident. It does not, and the reason is the framebuffer: a span writes 320
consecutive pixels, streaming through the 64 KB data cache and evicting whatever
shares a set with it. So the second read was taking cache misses in a loop, on a
32-byte object.

The fix is to stop reading it through the cache at all — hoist it once per
primitive into the state object, where it lives in registers and stack:

```c
// src_rebuild/PsyCross/src/gpu/PsyX_SoftRas.cpp:153
static inline void loadClut(int clut)
{
    st.clutX = (clut & 0x3F) << 4;
    st.clutY = (clut >> 6) & 0x1FF;

    const unsigned short* src = &vram[st.clutY * VRAM_WIDTH];
    unsigned nz = 0;
    for (int i = 0; i < 16; i++)
    {
        const unsigned short c = src[(st.clutX + i) & 1023];
        st.clut16[i] = c;
        if (c) nz |= 1u << i;      // 0x0000 is the PSX fully-transparent texel
    }
    st.clutNZ = nz;
}
```

Three milliseconds — the smallest step here, and worth being honest about: on its
own it is marginal. Its real value is that it makes the *next* step possible. You
cannot pre-modulate a palette you do not have in hand.

`clutNZ` is built here too: a bitmask recording which entries are non-zero, i.e.
which are opaque under the PSX rule that texel `0x0000` is fully transparent.
That bitmask is what keeps step 3 bit-identical.

### 2.5 Step 3 — pre-modulate the CLUT (106 -> 89 ms)

The second instructive one, and the one that most rewards reading the
disassembly.

PSX texturing modulates each texel by the primitive colour, with `0x80` meaning
identity. Driver 2 emits a great many flat-shaded, raw-textured polygons, so the
kernel had a hoisted flag for the identity case:

```c
const bool rawTex = (TEXFMT >= 0) && cr == 0x80 && cg == 0x80 && cb == 0x80;
...
if (GOURAUD)      pix = modulate(texel, r16 >> 16, g16 >> 16, b16 >> 16);
else if (rawTex)  pix = texel;
else              pix = modulate(texel, cr, cg, cb);
```

That reads like the raw case is free. The disassembly said otherwise: `rawTex` is
loop-invariant but not a compile-time constant, and `modulate()` is small,
branch-free and side-effect-free, so GCC if-converted the whole thing — it ran
`modulate()` **unconditionally** and used a conditional move to pick between its
result and `texel`. Six multiplies, six shifts and six clamps, computed and
discarded, on every pixel of every raw-textured polygon. About 30 cycles per
pixel, paid for nothing.

There is a way to make the cost vanish rather than making the branch work:
modulation of a 4bpp flat-shaded triangle depends only on the palette entry and
the primitive colour, both constant across the triangle. So apply it to the 16
entries once, and let the pixel loop do a table lookup:

```c
// src_rebuild/PsyCross/src/gpu/PsyX_SoftRas.cpp:433
// Flat-shaded 4bpp is the overwhelmingly common case in Driver 2, and there
// the per-pixel colour modulation depends only on the palette entry. Apply
// it to the 16 palette entries once per triangle instead: ~400 cycles here
// against ~30 cycles on every one of the span's pixels.
// FASTCLUT is a compile-time constant, so the other kernels lose nothing.
const bool FASTCLUT = (TEXFMT == 0) && !GOURAUD;
unsigned short clutPix[16];
if (FASTCLUT)
{
    for (int i = 0; i < 16; i++)
        clutPix[i] = rawTex ? s.clut16[i] : modulate(s.clut16[i], cr, cg, cb);
}
```

`FASTCLUT` is derived from template parameters, so it is a compile-time constant:
the specialised path exists only in the kernels where it applies, and the gouraud
and 8/15bpp kernels are not burdened with a dead branch. Break-even is about 13
covered pixels; Driver 2's flat 4bpp polygons cover 100-200.

**The detail that makes it correct.** Transparency previously tested the fetched
texel: `if (texel != 0)`. After pre-modulation you cannot test `clutPix[idx] != 0`
in its place, because modulation is not injective — a dark primitive colour can
drive a non-zero palette entry to zero, which would silently make an opaque texel
transparent. So the test moves to the raw palette entry, via the bitmask built in
step 2:

```c
// src_rebuild/PsyCross/src/gpu/PsyX_SoftRas.cpp:549
int idx = fetchIndex4(s, u16 >> 16, v16 >> 16);
// clutNZ tests the RAW entry, so transparency semantics are
// identical to the old `texel != 0`
unsigned short pix = clutPix[idx];
unsigned opaque = (s.clutNZ >> idx) & 1;
```

One shift and one AND of a register, replacing a comparison — and the output is
bit-identical to before, which is the property that let this ship without a visual
regression hunt.

Two smaller things rode along in the same build:

**Dead bounds masks.** The 4bpp fetch was masking its VRAM address with `& 511`
(row) and `& 1023` (column). Both are provably dead in that path, and the comment
records the proof rather than the conclusion:

```c
// src_rebuild/PsyCross/src/gpu/PsyX_SoftRas.cpp:235
if (FMT == 0) // 4-bit CLUT
{
    // texBaseY is 0 or 256 and v <= 255, so the row never leaves the page;
    // texBaseX <= 960 and u>>2 <= 63, so the column never wraps either.
    // Both of the old bounds masks were dead work.
    return s.clut16[(s.texPage[(v << 10) + (u >> 2)] >> ((u & 3) * 4)) & 0xF];
}
```

`v <= 255` holds because the PSX's mandatory 8-bit texture-coordinate wrap was
folded into the texture-window masks in `loadEnvState()` (`:138-141`): the window
masks are ANDed with `0xFF`, and the window offsets are at most 248, so the OR
can never carry past 255. That fold removed two more ANDs per fetch and made
these two masks dead in the same stroke.

**Hoisted texture page base.** `st.texPage` is computed once in `loadTpage()`
(`:150`), so the fetch indexes from a pointer instead of recomputing
`(texBaseY + v) * VRAM_WIDTH + texBaseX + ...`. `VRAM_WIDTH` is 1024, so the row
term collapses to `v << 10` and the whole address is one shift and one add.

Rasteriser cost after step 3: **68 cycles per covered pixel**, down from 221.

### 2.6 Step 4 — move the panel present to core 1 (89 -> 74 ms)

The present had been measured at 17 ms. Before optimising its conversion loop, do
the arithmetic on the transport:

> 320 x 240 x 2 bytes = 153,600 bytes = 1,228,800 bits.
> At the panel's 80 MHz SPI clock that is **15.4 ms** of pure transfer.

Measured 17. So at most 1.6 ms of that 17 was CPU — the pixel format conversion,
the nearest-neighbour rescale and the per-transaction overhead of the 15 DMA
chunks. The other 15.4 ms was the game task sleeping on a semaphore while the
DMA engine worked (`display_esplcd.c:303`, `xSemaphoreTake(sTransDone, ...)`).

That reframes the problem completely. There is no version of a faster conversion
loop that helps: the transfer is a hardware constant. What can be removed is the
*waiting*, by doing it somewhere that is not the critical path.

```c
// esp32_port/main/display_esplcd.c:222
// A full 320x240 16bpp frame is 153,600 bytes; at the panel's 80 MHz SPI clock
// that is 15.4 ms of pure transfer, and the measured present cost was 17 ms.
// So the present is transfer-bound, not CPU-bound: nearly all of it was the
// game task sitting on the DMA-complete semaphore. Handing the flush to a task
// on the other core takes that whole cost off the frame's critical path.
```

The safety argument is the interesting part. Handing a framebuffer to another
core to transmit while the game draws the next frame is only sound if the two
buffers are different memory — that is, if the game is genuinely
double-buffering. Rather than assume it, detect it, from the display origin
alternating between frames:

```c
// esp32_port/main/esp_platform.cpp:142
static int lastDy = -1;
const bool doubleBuffered = (lastDy >= 0 && lastDy != dy);
lastDy = dy;
...
if (doubleBuffered)
    displayPresentAsync(&vram[dy * VRAM_WIDTH + dx], VRAM_WIDTH, dw, dh);
else
    displayFlush555Scaled(&vram[dy * VRAM_WIDTH + dx], VRAM_WIDTH, dw, dh);
```

In game, `dy` toggles between 0 and 256 every frame. Driver 2's 640x512 frontend
does not alternate — both of its buffers sit at the same origin — so it takes the
blocking path and cannot tear. The check is per frame and costs one comparison.

The handoff also keeps the measurement honest. `displayPresentAsync()` takes the
idle semaphore *before* queueing the next job (`display_esplcd.c:281`), and that
call is inside the profiled window. If the flush ever became the longer of the
two (frame time dropping below ~15.4 ms, or a larger panel), the wait would
reappear in the `present` field instead of silently stalling. Today it reads 0.

### 2.7 Step 5 — rasterise on both cores (74 -> 59 ms)

Core 1 was doing 15.4 ms of DMA supervision per frame and idling the rest. The
remaining large item was rasterisation, so: both cores walk the **same** ordering
table, each restricted to a horizontal band of VRAM.

```c
// src_rebuild/PsyCross/src/gpu/PsyX_GPU.cpp:849
// Both cores run the identical walk, each restricted to its own band of
// the screen (see esp_raster.cpp). Every pixel is written by exactly one
// core, so the ordering table's painter ordering is preserved within
// each band.
if (g_softRasterEnabled && SoftRas_ForkEnabled())
{
    SoftRas_ForkOT(p);      // hand the same list to the worker on core 1
    SR_WalkOT(p);           // this core takes the top band
    SoftRas_JoinOT();
    return;
}
```

Banding rather than splitting the primitive list is forced by the algorithm.
Driver 2 has no depth buffer; correctness comes from painter's ordering through
the reverse ordering table. Give each core half the primitives and that ordering
is destroyed. Give each core half the *screen* and it is preserved trivially: each
core still visits every primitive in list order, and every pixel is written by
exactly one core.

Three pieces of state had to stop being global for this to be sound:

1. **The render state.** `SRState st` is rebuilt for every primitive, and the two
   cores are at different points in the list at any instant, so one core would
   rasterise with the other's texture page and CLUT. It became
   `__thread` (`PsyX_SoftRas.cpp:86-90`).
2. **The draw environment.** `DR_TPAGE`, `DR_TWIN`, `DR_AREA` and the SCE
   null-polygon tpage hack mutate it as the walk proceeds. It became
   `g_drawEnv[2]`, with `activeDrawEnv` a macro selecting by core id
   (`PsyX_GPU.h:29-37`). Both cores replay the identical packet stream into their
   own slot, so both must end bit-identical — checked at every join, reported as
   `diverge`, and observed to be zero:

   ```c
   // esp32_port/main/esp_raster.cpp:114
   if (memcmp(&g_drawEnv[0], &g_drawEnv[1], sizeof(DRAWENV)) != 0) {
       g_srEnvDiverge++;
       g_drawEnv[1] = g_drawEnv[0];
   }
   ```
3. **Primitives with side effects outside the band.** Two occur in game and both
   go through a rendezvous so they happen exactly once: `sky.c` blits a rect of
   the framebuffer to sample the sun, which *reads* pixels both cores are writing
   (`PsyX_GPU.cpp:1633`); and `objanim.c` uploads cycled CLUTs via `DR_LOAD`,
   which would tear under the other core's in-flight fetches
   (`PsyX_GPU.cpp:1678`). About thirteen per frame — negligible.

The split is not fixed at half, because the load is not. The top of a Driver 2
frame is sky and the bottom is road, so the split line moves each frame toward
whichever core drew fewer span pixels, capped at 32 lines and skipped on
near-empty frames so it cannot oscillate:

```c
// esp32_port/main/esp_raster.cpp:121
const int p0 = (int)g_srBandPx[0], p1 = (int)g_srBandPx[1];
if (p0 + p1 > 4096) {
    sSplitY -= (p0 - p1) * 32 / (p0 + p1);
    ...
}
```

It settles at 29 vs 32 ms. That the balancer works is visible directly in the
`raster c0 + c1` field — the same per-core counters from §1.2, now doing a second
job.

Correctness was verified by comparison, not inspection: the dual-core frame is
byte-identical to the single-core render, **0 of 19200 sampled pixels differ**.
`g_srForkEnabled` (`esp_raster.cpp:44`) is a runtime kill switch back to single
core, and every wait carries a 200 ms timeout that logs and continues, so a future
divergence degrades to a glitched frame rather than a hang.

**Why 1.47x and not 2x.** Raster time went from 47 ms to 32 ms. (47 ms of raster
plus 27 ms of everything else is the 74 ms frame that step 4 left behind; 32 + 27
is the 59 ms frame it becomes.) Three reasons, in decreasing order of size:

- Per-primitive setup is *duplicated*, not divided. Both cores decode every
  primitive, and `loadTpage()`/`loadClut()` run before any band test
  (`PsyX_SoftRas.cpp:807` and its siblings) — so the 16-halfword CLUT pull happens
  twice for every textured primitive, including the ones that miss the core's
  band entirely. Only the covered-pixel work is genuinely split.
- The band balance settles at 29 vs 32, about 10% imbalance.
- Game logic and the software GTE (27 ms) are not parallelised at all, so
  Amdahl's law caps the frame-level gain regardless.

---

## 3. What was measured and rejected

This is the most valuable section, because both of these were plausible, both
were on the shortlist, and both would have cost days. A measurement that stops
you doing something is worth as much as one that speeds you up.

### 3.1 Framebuffer writes to PSRAM are free — so band rendering was dropped

The obvious suspicion about a software rasteriser whose framebuffer lives in
external RAM is that it is memory-bound. The standard remedy is a band or tile
renderer: rasterise into a small buffer in fast internal SRAM, then blit each
finished band out to the real framebuffer, so the pixel loop only ever touches
fast memory.

That is a large redesign. It needs the primitive stream binned by band, which
fights painter ordering; it needs internal SRAM that this port does not have
(~20 KB free after the app); and it complicates the dual-core split. So before
building it, `SOFTRAS_FBSCRATCH` was used to measure the *upper bound* on what it
could possibly win: redirect every span write to a single 1024-entry row in
internal SRAM, which is strictly better than any real band renderer could ever be
(it is not even a correct image — it just measures the writes).

> **96 cycles per pixel with the PSRAM framebuffer. 95 with internal SRAM.**

One cycle. Within noise. The inner loop is **ALU-bound, not memory-bound**, and
the band-rendering redesign was dropped on the spot.

Why this is the expected answer once you look at it: span writes are sequential
halfwords, so they hit the same 64-byte cache line 32 times in a row; the cache
is write-back, so the PSRAM sees whole-line writebacks rather than individual
stores, absorbed behind the CPU; and the framebuffer is read exactly once per
frame, by the present. The pixel loop's remaining cost is arithmetic and the
*texture* side of memory — which is the read that `SOFTRAS_NOTEX` showed at ~34
cycles per pixel, half the total, and which a band renderer does not help with
at all.

The measurement cost one `#ifdef` and one build. The redesign it prevented would
have cost days and delivered 1%.

### 3.2 The present is transfer-bound — so its conversion loop was left alone

Same shape of reasoning, reached from the arithmetic in §2.6: 15.4 ms of the
17 ms present is SPI transfer at 80 MHz. The per-pixel conversion is
`psx555toPanel()` (`display_esplcd.c:187-195`), called from the scaling loop
(`display_esplcd.c:305-311`) into ping-pong DMA buffers. It moves red from the
low bits of the PSX's BGR555 word up to bits 11-15 of RGB565, widens green from 5
bits to 6 by replicating its top bit, and byte-swaps the result because the panel
wants MSB first. A handful of shifts, some masks and an OR, in plain C.

Making that loop twice as fast would have recovered under 1 ms of a 17 ms
operation, and the 15.4 ms would still have been there. So it was not touched. It
is still plain C today, deliberately — and the whole 17 ms was removed instead by
moving where it happens, not what it does.

(The red-channel move in that function is not decoration. It was added later,
when a review found that the port had been leaving red in the low bits and
relying on a MADCTL colour-order bit that is clear on this board — see
`04-rasterizer-correctness.md` and `05-the-esp32-platform-layer.md`. It costs one
no extra instructions — it changes which operand is shifted, not how many shifts
run — and, per the paragraph above, the whole conversion is not measurable
against 15.4 ms of SPI.)

The corollary is worth stating for anyone continuing this work: **the present is
not on the table as an optimisation target until the frame time approaches
15.4 ms.** Below that, the SPI transfer becomes the frame's floor, and the only
remaining levers are the panel clock or a narrower pixel format.

---

## 4. Where the time goes now

At 16.8 fps, a 59 ms frame:

| Component | Time | Notes |
| --- | ---: | --- |
| Rasterisation (slower band) | 32 ms | critical path; the other core's band is 29 ms |
| Game logic + software GTE + OT walk | 27 ms | the `other` field, by subtraction |
| Panel present | 0 ms | 15.4 ms of DMA, entirely on core 1, off the critical path |

And inside the rasteriser, per covered pixel:

| | Cycles/px | Source |
| --- | ---: | --- |
| Total, at the start | 221 | measured |
| Total, now | 68 | measured |
| — of which texture fetch | ~34 | `SOFTRAS_NOTEX` ablation |
| — of which framebuffer writes | ~1 | `SOFTRAS_FBSCRATCH` ablation |

The workload itself, measured on the real primitive stream:

| Surface | Polygon size | Texels per pixel |
| --- | --- | --- |
| ground, buildings, foliage | 100-200 px | 1.2 - 1.6 |
| the player car | 2-3 px | 11 - 23 |

Frames carry ~1200 primitives standing still and 2300-3100 while driving.

### Next targets, in the order the measurements suggest

1. **Stop duplicating per-primitive setup across the two cores.** `loadTpage()`
   and `loadClut()` run before any band test, so every textured primitive pays a
   16-halfword CLUT pull on both cores, even when its bounding box misses one
   core's band entirely. A cheap band-vs-bounding-box reject before `loadClut()`
   would remove that. This is arithmetic, not a measurement — it should be
   profiled before it is believed, but it is the largest identified inefficiency
   in the dual-core split and it explains most of the gap between the 1.47x
   observed and the 2x ideal.
2. **The 27 ms of game logic and software GTE.** It is now nearly half the frame
   and has never been profiled at all — the instrumentation reports it only by
   subtraction. The GTE is emulated in portable C
   (`src_rebuild/PsyCross/src/gte`); on a machine with no hardware divider and
   no SIMD, the fixed-point transform and lighting paths are the obvious place to
   look, and the first job is simply to bracket them the way §1.1 brackets the
   rasteriser.
3. **The texture fetch, at ~34 of the 68 cycles per pixel.** The 4bpp path is
   already down to one shifted load plus a nibble extract plus a table lookup, so
   the remaining cost is PSRAM read latency on a dependent chain. Anything that
   improves it has to improve *locality*, not instruction count — the candidates
   are keeping hot texture pages in internal SRAM, or reordering primitives by
   texture page, and the second conflicts with painter ordering.
4. **Not the present.** See §3.2.

### The trade-off that is deliberately not taken

The 16.8 fps figure is the unfiltered rasteriser. There is an optional texture
minification filter (`g_srFilter`, `PsyX_SoftRas.cpp:446-492`) which samples each
pixel's footprint on a 2x2 or 4x4 rotated grid when a triangle is minifying,
because the PSX's one-texel-per-pixel sampling turns compressed geometry into
noise on a sharp LCD in a way a CRT hid. It is a real quality improvement and it
is expensive.

The cost was measured by toggling the filter live with the `f` key: the same
scene ran at **14.5 fps with the filter off and 10.0 fps with it on**. That
comparison is internally valid — both halves are the same scene seconds apart —
but **14.5 fps is the baseline of that particular A/B scene, not the 16.8 fps
headline above.** The two runs were made in different parts of the level, and per
the caveat in §2 the frame load varies a great deal with what is on screen. Never
read 14.5 as "the" unfiltered rate.

With the filter on and the car actually moving, the measured rate is 5-9 fps.
Raising the threshold to 2.5 texels per pixel (`0333687`) recovered part of that.

It is a toggle rather than a decision: `f` on the serial console flips it live so
the two can be compared on the panel without reflashing
(`esp_platform.cpp:310-314`). That is the honest state of it — the port has a
sharpness-versus-framerate knob and no single right position for it.
