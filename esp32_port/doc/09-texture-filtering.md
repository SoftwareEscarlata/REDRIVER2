# Why it looks noisy, and the minification filter

The first time Driver 2 rendered a real Havana street on the ESP32-S3 panel, the
geometry was unmistakably correct — roads, kerbs, buildings, palm trees, the car
— and the *surfaces* were garbage. Not wrong-coloured, not torn: statistically
noisy. Every road strip and every building face was a fizzing carpet of
unrelated pixels that changed completely when the camera moved a few
centimetres.

This document is the investigation that followed, the thing it turned out to be,
the filter that was written for it, and an honest account of what that filter
does and does not fix.

All code references are to
`E:/Hardware/Driver2/REDRIVER2/src_rebuild/PsyCross/src/gpu/PsyX_SoftRas.cpp`
unless otherwise stated, and to
`E:/Hardware/Driver2/REDRIVER2/esp32_port/main/esp_platform.cpp` for the
device-side debug hooks.

---

## 1. The first hypothesis was wrong

Noise that looks like *this* — high-frequency, uncorrelated, confined to
textured surfaces — is exactly what you get when the texture data itself is
corrupt. And there was every reason to suspect it. The path from the level file
to a texel on this port is long and every stage of it had been rewritten:

- the level's texture pages are RNC-decompressed by the game's portable C
  (`Game/ASM`), on a CPU that is not a MIPS R3000A,
- uploaded through `LoadImage` into an emulated 1024x512 VRAM array that lives
  in **octal PSRAM**, not in a real GPU,
- and read back by a from-scratch software rasteriser through a 4bpp CLUT
  indexing path with a texture-window mask folded into it
  (`fetchTexel<0>`, line 240).

Any of those could have shredded the data. So the first job was to prove or
disprove it, and the only way to do that on a board with no debugger attached to
VRAM is to get the bytes off the device.

### 1.1 Getting VRAM off the board

Four single-keystroke dump modes were added to the serial console, in
`SoftRas_PresentESP` (`esp_platform.cpp:102-132`). They fire once, at end of
frame, when the corresponding key arrives:

| key | mode | what it produces |
| --- | --- | --- |
| `p` | panel | the display rect, downscaled to 160x120, as RGB332 hex |
| `v` | VRAM | all 1024x512 of VRAM, half scale, as RGB332 hex |
| `r` | raw | **16-bit VRAM words**, verbatim, of one texture page |
| `t` | decoded page | one 4bpp page decoded 1:1 through the rasteriser's own sampler |

The distinction between `v` and `r` is the whole point. `p` and `v` are lossy
by construction — they crush 5-5-5 to 3-3-2 and decimate — so they can show you
*that* something looks wrong but can never prove anything is right. `r` is
bit-exact:

```c
} else if (sDumpRequest == 4) {          // 'r': raw VRAM words of one tpage
    sDumpRequest = 0;
    printf("\n#RAW 448 0 64 256\n");
    for (int y = 0; y < 256; y++) {
        char line[257];
        static const char hex[] = "0123456789abcdef";
        for (int x = 0; x < 64; x++) {
            const uint16_t w = vram[y * VRAM_WIDTH + 448 + x];
            line[x * 4 + 0] = hex[(w >> 12) & 15];
            ...
        }
        printf("%s\n", line);
        vTaskDelay(1);
    }
```

The `vTaskDelay(1)` per line is not cosmetic: 256 lines of 256 hex characters at
115200 baud will overrun the USB-JTAG console buffer and silently drop rows,
which would have produced a *fake* corruption to chase.

`t` is the more interesting one, because it removes the last degree of freedom.
`SoftRas_DebugDecodeTPage` (line 755) decodes a page **with the rasteriser's own
sampler**, not with a separate reference decoder:

```c
    // 1:1 so fine 4bpp detail survives — decimating here would look like noise
    for (int v = 0; v < 128; v++)
        for (int u = 0; u < 128; u++)
            out[v * 128 + u] = fetchTexel<0>(st, u0 + u, v0 + v);
```

It sets up `st` by hand, calls the identical `fetchTexel<0>` the inner loop
calls, and restores `st` afterwards. If the CLUT indexing, the nibble
selection, the texture-window mask or the page base were wrong, this dump would
be wrong in exactly the same way the frame is. The comment on the loop is a
scar: the first version of this decimated 256x256 down to 128x128 and *the
decimation itself reproduced the noise*, which is the joke the whole
investigation eventually turned on.

### 1.2 The result

The raw words from `r` were decoded offline and compared against the same page
as it exists in `data/DRIVER2DEMO/LEVELS/HAVANA.LEV`. **Pixel-identical.** Not
approximately, not modulo a palette rotation — identical.

The `t` dump of the scenery page at tpage base (704, 0) with the CLUT at
(960, 294) came back as a clean, legible 4bpp atlas: recognisable brickwork,
window frames, foliage, road markings, all sharp.

In parallel the per-primitive parameters were logged straight out of
`SoftRas_Primitive` — `tpage`, `clut`, the three or four UV pairs, the format
bits — and checked against what the geometry should be asking for. They were
correct. The right page, the right palette, plausible in-range UVs, format 0
(4bpp) as expected.

So: the texture data was perfect, the sampler addressed it correctly, and the
frame still looked like static. The data was never the problem.

---

## 2. What it actually is

The PlayStation GPU takes **exactly one texel per pixel**. No filtering, no mip
levels, no perspective correction — the rasteriser here is faithful to that, and
says so at the top of the file:

```c
//   interpolation  = affine (PSX had no perspective correction)
//   ordering       = caller draws far-to-near via the reverse OT (painter's)
//   pixels         = 15bpp  r | g<<5 | b<<10 | stp<<15
```

That is fine when one screen pixel covers roughly one texel. It stops being
fine the moment the geometry recedes. A road strip fifty metres away compresses
a detailed 256x256 atlas by several times in each direction; the pixel's
**footprint** in texture space is then a parallelogram many texels across, and
the single sample the hardware takes is, for practical purposes, a uniform
random draw from inside it. Move the camera by a fraction of a pixel and you
draw a different element. That is not an artefact that resembles noise — it *is*
noise, in the precise sense: an unbiased single-sample estimate of a
high-variance quantity.

The PSX got away with it. It rendered 320x240 into a composite signal on a CRT,
and the CRT's spot size, the composite bandwidth and the phosphor decay are
collectively a low-pass filter with a kernel comfortably wider than a pixel. The
console shipped with a reconstruction filter it did not have to pay for. The
comment in the filter block puts it plainly (lines 449-452):

```c
    // atlas by 3-10x, so that single sample is essentially a random pick out of
    // the footprint and the surface turns to noise. A CRT low-passed it away;
    // a sharp LCD does not. So when a triangle is minifying, sample the pixel's
    // actual footprint on a 2x2 rotated grid and average.
```

An ST7789 LCD reproduces each pixel as a hard-edged rectangle of uniform colour.
It is a nearly ideal display and it shows you the aliasing at full strength.

### 2.1 Confirming it offline

Rather than argue from theory, the hypothesis was tested outside the firmware.
One textured span was logged from the device — its UV endpoints, its screen
extent, its tpage and CLUT — and the same span was rasterised in Python against
the decoded atlas, twice: once with point sampling (what the PSX and the
rasteriser do), once with a box filter over the footprint.

**Point sampling reproduced the on-panel noise exactly** — the same character,
the same spatial frequency, visibly the same failure. The box filter produced a
clean, slightly soft surface. That closed the question: the sampling rule was
the entire cause, and averaging over the footprint was the entire fix.

(These scripts were throwaway and are not in the repository. The device-side
hooks that fed them — `r`, `t`, and the per-primitive log — are.)

---

## 3. Measuring the footprint

Before writing a filter it was worth knowing how bad the compression actually
is, and where. That is what the `SOFTRAS_DEBUG_CAR` block does (lines 404-425).
It is compiled out of the shipped firmware — `esp32_port/main/CMakeLists.txt:73`
defines only `SOFTRAS_PROFILE` — and was enabled for one measurement run:

```c
    if (TEXFMT >= 0 && st.clipW320)
    {
        const int cx = (v0.x + v1.x + v2.x) / 3, cy = (v0.y + v1.y + v2.y) / 3;
        if (cx > 110 && cx < 215 && cy > 120 && cy < 215) {
            ...
                const int su = (duDx < 0 ? -duDx : duDx) + (duDy < 0 ? -duDy : duDy);
                const int sv = (dvDx < 0 ? -dvDx : dvDx) + (dvDy < 0 ? -dvDy : dvDy);
                PsyX_Log("CAR fmt=%d gour=%d xy=(%d,%d)-(%d,%d) ... "
                         "texels/px u=%d.%02d v=%d.%02d", ...
```

`duDx`, `duDy`, `dvDx`, `dvDy` are the affine attribute gradients in 16.16 per
screen-pixel step, set up at lines 375-378. The quantity being printed,

```
su = |du/dx| + |du/dy|
sv = |dv/dx| + |dv/dy|
```

is not an approximation of anything — it is **exactly** the u-extent and
v-extent of the pixel's footprint parallelogram. Stepping across the pixel
square from corner to corner changes `u` by `du/dx + du/dy` at most, so `su` is
the width of the footprint's axis-aligned bounding box in texture space, in
texels. That is the number you want: it is how many distinct texels the pixel
should have averaged and did not.

The gate at `cx/cy` restricts the log to the lower-centre of the 320x240
viewport, where the player car sits, so the sample covers the car *and*
whatever near-field scenery is behind it. The result was two populations that
could not be more different:

| what | polygon size | footprint (texels/pixel) |
| --- | --- | --- |
| ground, buildings, foliage (near field) | 100-200 px | 1.2 - 1.6 |
| the player car | 2-3 px | 11 - 23 |

The car is the extreme case and it is extreme for a reason that has nothing to
do with distance. It is a *detailed* model — a full car's worth of texture
across a body that occupies a couple of hundred pixels on a 320-wide panel — so
its individual polygons are two or three pixels each while spanning twenty-odd
texels. The ratio is high because the model is dense, not because it is far.

Far-field ground and buildings are not in that table because the probe window
sits over the near field, but they are the same atlas pages seen at several
times the compression, and they are what the noise was most visible on.

This split is the entire design constraint. A kernel wide enough for the car
would be catastrophically expensive if applied to a road strip; a kernel cheap
enough for the road does almost nothing for the car.

---

## 4. The adaptive kernel

The setup runs once per triangle, immediately after the pre-modulated CLUT is
built (lines 457-492):

```c
    int offU[16], offV[16];
    int taps = 0;
    if (FASTCLUT && g_srFilter)
    {
        const int su = (duDx < 0 ? -duDx : duDx) + (duDy < 0 ? -duDy : duDy);
        const int sv = (dvDx < 0 ? -dvDx : dvDx) + (dvDy < 0 ? -dvDy : dvDy);
        const int worst = su > sv ? su : sv;

        int side = 0;
        if (worst > 0x28000) side = 2;
        if (worst > 0x40000 && (maxX - minX + 1) * (maxY - minY + 1) <= 64) side = 4;

        if (side)
        {
            taps = side * side;
            const int shift = (side == 2) ? 2 : 3;      // divide by 2*side
            for (int j = 0, k = 0; j < side; j++) {
                const int dj = 2 * j + 1 - side;
                for (int i = 0; i < side; i++, k++) {
                    const int di = 2 * i + 1 - side;
                    offU[k] = (duDx * di + duDy * dj) >> shift;
                    offV[k] = (dvDx * di + dvDy * dj) >> shift;
                }
            }
        }
    }
```

`worst` is `max(su, sv)`, the longer side of the footprint's bounding box. The
two constants are 16.16:

| constant | texels/pixel | effect |
| --- | --- | --- |
| `0x28000` | 2.5 | below this, no filtering at all |
| `0x40000` | 4.0 | above this **and** bbox ≤ 64 px, widen to 4x4 |

> **A note on a stale comment.** The comment above this block opens with "2x2
> past 1.5 texels per pixel", and the commit message for this change
> (`feaaeb2`, "softras: filter minified textures") says 1.5 as well. The shipped
> constant is `0x28000`, which is **2.5**, and the second half of the same
> comment block says 2.5. The threshold was raised during tuning and the first
> sentence was not updated. Trust the constant.

The 2.5 gate is why the 1.2-1.6 near-field population is deliberately left
alone. At that ratio the single sample already lands close to the footprint
average and a box filter mostly just removes detail that was legitimately there
— and it would cost the most, because that population is the one covering
hundreds of pixels per polygon. The gate buys the surface area back.

### 4.1 Why the size gate is what makes 4x4 affordable

The `(maxX - minX + 1) * (maxY - minY + 1) <= 64` condition on the wide kernel
is the load-bearing part of the design, and it works because of the two
populations above.

Sixteen taps is four times the work of four taps. Applied per-pixel to a road
strip it would be ruinous. But the ratios that call for sixteen taps occur
almost exclusively on *small* polygons — the car — so gating on bounding-box
area separates the two cases with a single cheap integer test that was already
computed for the raster bounds.

The budget works out like this:

| case | pixels per triangle | taps | texel fetches per triangle |
| --- | --- | --- | --- |
| road strip, 2x2 | 100-200 | 4 | 500 - 1000 |
| car polygon, 4x4 | ≤ 64 (bbox) | 16 | ≤ 1088 |

(Each pixel fetches its centre sample as well as its taps, so the multiplier is
`taps + 1`: 5 for the 2x2 case, 17 for the 4x4.)

A 4x4-filtered car polygon costs about what a 2x2-filtered road strip costs, in
total, because the size cap bounds the multiplier. Without the cap, a distant
road strip that crossed 4.0 texels/pixel — which happens constantly — would take
sixteen taps across two hundred pixels and the frame rate would collapse.

The cap is on the **bounding box**, not on covered pixels, so it is
conservative: a sliver triangle with a large bbox and few covered pixels is
denied the wide kernel. That is the safe direction to err.

---

## 5. Where the samples go

The offsets are not an arbitrary jitter pattern. For a grid of `side` samples
across one pixel, the cell centres of a box filter sit at

```
(2*i + 1 - side) / (2 * side)     for i = 0 .. side-1
```

which is exactly what the loop computes: `di = 2*i + 1 - side`, then a shift by
`log2(2*side)` — 2 for `side == 2`, 3 for `side == 4`. So

- **2x2**: screen-space offsets of ±1/4 pixel — the "quarter-step" the design is
  named after,
- **4x4**: ±1/8 and ±3/8 pixel, spacing still a quarter pixel.

Both grids are regular in *screen* space. What makes them useful is that they
are transformed into texture space by the same Jacobian the rasteriser is
already carrying:

```c
offU[k] = (duDx * di + duDy * dj) >> shift;
offV[k] = (dvDx * di + dvDy * dj) >> shift;
```

`(duDx, dvDx)` and `(duDy, dvDy)` are the columns of the UV Jacobian. Applying
them to a square screen-space grid produces a sample pattern that is **rotated
and stretched into the footprint's own parallelogram** — anisotropic, for free,
with no extra state.

This matters more than it sounds. On the road surface the compression is an
order of magnitude stronger vertically than horizontally: the ground plane is
seen at a grazing angle, so a screen row covers a short strip of world and a
screen column covers a long one. A square kernel in texture space would either
over-blur along `u` or under-sample along `v`. Taking the shape from the
Jacobian gets both axes right by construction.

The whole setup is `side*side` multiply-add pairs and two shifts, once per
triangle — a few hundred cycles against a span of hundreds of pixels.

---

## 6. Transparency, and the bit that had to be carried

The filtered path lives inside the `FASTCLUT` branch of the inner loop
(lines 547-590):

```c
                else if (FASTCLUT)
                {
                    int idx = fetchIndex4(s, u16 >> 16, v16 >> 16);
                    unsigned short pix = clutPix[idx];
                    unsigned opaque = (s.clutNZ >> idx) & 1;

                    if (taps)
                    {
                        int ar = 0, ag = 0, ab = 0;
                        unsigned all = 1;
                        for (int k = 0; k < taps; k++) {
                            const int ik = fetchIndex4(s, (u16 + offU[k]) >> 16,
                                                          (v16 + offV[k]) >> 16);
                            all &= (s.clutNZ >> ik) & 1;
                            const unsigned short c = clutPix[ik];
                            ar += c & 31; ag += (c >> 5) & 31; ab += (c >> 10) & 31;
                        }
                        if (all) {
                            const int sh = (taps == 4) ? 2 : 4;
                            pix = (unsigned short)((ar >> sh) | ((ag >> sh) << 5)
                                                              | ((ab >> sh) << 10)
                                                  | (clutPix[idx] & 0x8000));
                            opaque = 1;
                        }
                    }
```

Three details are worth pulling apart.

### 6.1 Averaging happens on already-modulated colours

`clutPix[]` is the 16-entry palette with the triangle's flat colour modulation
already applied (lines 438-444) — an optimisation from the earlier tuning pass
that moved `modulate()` from per-pixel to per-triangle. The filter inherits it
for free: each tap is one 4bpp index fetch plus a table lookup, with no
per-tap modulation. Since flat modulation is a per-triangle constant, averaging
after it is equivalent to averaging before it, up to the channel clamps.

`fetchIndex4` (line 257) is the reason this is cheap. It returns the palette
*index*, not the colour, so a tap costs a texture-window mask, one PSRAM
half-word read and a nibble extract — no CLUT indirection.

### 6.2 Blend only where the whole footprint is opaque

PSX 4bpp textures encode full transparency as palette entry `0x0000`
(`loadClut`, line 164, builds the `clutNZ` bitmask for exactly this test). The
foliage, fences and railings in Havana are cutout geometry: a rectangle of
texture where most texels are the transparent entry and the visible shape is
cut out of it.

Averaging across a cutout edge would mix the transparent entry's colour —
black — into the silhouette, producing a dark fringe around every palm frond
and every railing. So the filter refuses:

```c
all &= (s.clutNZ >> ik) & 1;
...
if (all) { /* use the average */ }
/* else pix and opaque keep their centre-sample values */
```

If any tap is transparent, the pixel falls back to the plain PSX centre sample.
Interiors get filtered; edges stay hard and correct. The cost is that cutout
silhouettes are the one place the noise survives — an accepted trade, because a
fringed silhouette is a far more objectionable artefact than a slightly noisy
leaf.

There is one honest wrinkle here: `all` covers the *taps*, not the centre
sample. So a pixel whose centre texel is transparent but whose entire tap set is
opaque — a sub-pixel hole in otherwise solid texture — gets written, where real
hardware would have skipped it. `opaque = 1` is forced in that path. In
practice this fills pinholes smaller than a pixel, which is arguably what a
filter *should* do, but it is a deviation from PSX semantics and it is
deliberate only in the sense that nobody minded.

### 6.3 The semi-transparency bit

This one cost a visible bug before it was found. The average is rebuilt from
the three 5-bit colour channels only:

```c
pix = (ar >> sh) | ((ag >> sh) << 5) | ((ab >> sh) << 10)
```

Bit 15 — the PSX STP (semi-transparency) flag — is not a colour channel and
cannot be averaged, so it has to be carried explicitly:

```c
| (clutPix[idx] & 0x8000)
```

taken from the centre sample. It survives `modulate()` (line 211 ORs
`texel & 0x8000` back in), so `clutPix[]` still holds it.

Without that term, the downstream test

```c
if (SEMI && (pix & 0x8000))
    row[x] = blendPixel(row[x], pix, s.abr);
else
    row[x] = pix & 0x7FFF;
```

took the `else` branch on every filtered pixel of a semi-transparent primitive.
The most conspicuous casualty was **the car's shadow, which rendered as an
opaque black rectangle** under the vehicle. It is a good example of what a
faithful-reimplementation port keeps doing to you: an averaging filter is a
purely local, obviously-safe operation right up to the point where the pixel
format has a non-numeric bit in it.

---

## 7. Scope: what is actually filtered

The filter is gated on `FASTCLUT`, which is

```c
const bool FASTCLUT = (TEXFMT == 0) && !GOURAUD;
```

— **flat-shaded 4bpp only**. That covers the overwhelming majority of Driver 2's
world geometry (the reason the `FASTCLUT` fast path exists at all), but it means
the following get no filtering whatsoever:

- gouraud-shaded textured polygons (`POLY_GT3` / `POLY_GT4`),
- 8bpp and 15bpp textures,
- sprites and tiles, which go through `drawSprite` / `drawTile` on their own
  axis-aligned paths,
- magnification — there is no bilinear path, only minification averaging.

Extending it to the gouraud kernel is mechanically easy but would need
per-tap `modulate()`, since the colour is no longer constant across the
triangle, which is precisely the ~30 cycles per pixel the earlier optimisation
pass removed. It has not been done.

(`avg2()` at line 194 — a branch-free two-pixel average — is a leftover from an
earlier two-tap prototype and is currently unused.)

---

## 8. What it costs, and the toggle

Measured on the board, in game, stationary, by toggling the filter live with the
`f` key so that both halves are the same scene seconds apart:

| | frame time | fps |
| --- | --- | --- |
| filter off | 69 ms | 14.5 |
| filter on | 100 ms | 10.0 |

> **Read the 14.5 correctly.** It is the baseline *of this particular A/B scene*,
> not the port's headline unfiltered frame rate, which is **16.8 fps**. The two
> were measured in different parts of the level: this A/B ran in a heavier one.
> Frame load varies a lot — roughly 1200 primitives standing still against
> 2300-3100 while driving — so an absolute fps figure only means anything next to
> the scene it came from. What the pair above is good for is the *ratio*, which is
> exactly what a live toggle in one scene measures well.

That is **+31 ms per frame, about +45%**, on the critical path — which for this
port is the slower of the two core bands (see the dual-core work: the frame
waits for both cores, so the cost lands wherever the filtered polygons are).
While driving, with the filter on, the frame rate sits between **5 and 9 fps**,
because the mix of near and far geometry — and therefore the fraction of the
screen that crosses the 2.5 gate — changes constantly with the camera.

The reference numbers this sits on top of, all measured on the device:

- rasteriser cost after the optimisation pass: **68 cycles per covered pixel**
  (down from 221),
- of which the texture fetch is about **34 cycles per pixel**, established by
  stubbing the fetch out (`SOFTRAS_NOTEX`, line 223),
- framebuffer writes to PSRAM are effectively **free** — redirecting them to
  internal SRAM changed nothing (96 vs 95 cyc/px, via `SOFTRAS_FBSCRATCH`,
  line 283). The inner loop is ALU-bound, not memory-bound.

That last point is why four extra taps cost what they cost: they are four more
trips through the ALU-heavy fetch, not four more memory stalls.

Because that is a large price for an optional improvement, it is a runtime
switch rather than a build option. `g_srFilter` (line 96) defaults to on and is
flipped from the serial console (`esp_platform.cpp:310-314`):

```c
            if (buf[i] == 'f') {
                g_srFilter = !g_srFilter;
                ESP_LOGI(TAG, "texture filter %s", g_srFilter ? "ON" : "off");
                continue;
            }
```

You can therefore A/B it on the panel, in a live frame, without reflashing —
which is the only way to judge a filter honestly. The fps line printed every
60 frames (`esp_host.cpp:86-93`) gives you the cost at the same time.

One practical wrinkle: `drive.ps1` forwards only `P V T R` as one-shot debug
keys (`drive.ps1:60`), so `f` has to be typed into a plain terminal on the port,
not into the keyboard-driver window.

---

## 9. Honest assessment of the result

**Ground, buildings and foliage improve a great deal.** The road stops fizzing.
Building faces read as brick and window instead of as static. Distant geometry
becomes stable under camera motion, which is the part that mattered most —
temporal noise is far more distracting than spatial noise. On these surfaces the
filter does close to what the offline box-filter reference did.

**The car improves much less.** Sixteen samples out of a footprint measured at
11-23 texels across is still sparse: a 4x4 grid over a 23x7 texel region samples
roughly one texel in ten. The variance drops — it is a genuine four-fold
reduction in noise power over the single sample — but it does not converge, and
the car's bodywork still shimmers under motion. The kernel was widened as far as
the cost model allows and it is simply not wide enough for that compression
ratio.

**Cutout silhouettes are unfiltered by design** (section 6.2), so palm fronds and
railings keep their aliasing. That is the right trade, but it is a limitation.

**Only flat 4bpp is covered** (section 7).

### What a proper fix would need

Mip levels. Averaging N taps per pixel is an O(N) approximation to something
that should be O(1): pre-filter the texture once, at load time, into a pyramid,
and select the level from the same `worst` value the kernel already computes.
That is the standard answer and it would be both cheaper *and* better than any
tap count reachable here.

It cannot be done straightforwardly for this content, and the reason is
fundamental rather than an engineering inconvenience:

**A 4bpp CLUT texture cannot be mipped in index space.** The stored value is a
*palette index*, and the average of two indices is not the index of the average
of two colours — index 3 and index 7 might be dark red and pale blue, and index
5 might be green. Averaging indices produces arbitrary colours. To build a
correct mip level you must decode to RGB, filter, and then re-encode — and
re-encoding needs a palette.

Which forces the awkward part: the mip levels would have to be built and stored
**per palette**, not per page. Driver 2's texture pages are shared across many
CLUTs — that is the point of a 4bpp indexed atlas, and `objanim.c` even cycles
CLUTs at runtime for animated surfaces. So the storage is not "one pyramid per
page" (which would be a manageable +33%) but "one pyramid per (page, palette)
pair", and the natural output format is 15bpp direct colour, four times the
bits per texel of the source. Against 8 MB of PSRAM already holding a 1 MB VRAM
array plus the game's working set, and a device with roughly 20 KB of internal
SRAM free, that is not a decision to make casually.

The alternatives — quantising each mip level back to the same 16-entry palette
(cheap, but the palettes are chosen for the full-resolution image and averaged
colours frequently fall outside their gamut), or building pyramids lazily only
for the (page, palette) pairs a level actually uses — are both plausible and
neither has been tried. As it stands, the adaptive kernel is what is shipped: it
fixes the surfaces that were worst affected, it costs about a third of the frame
budget, and it is one keypress away from being turned off.
