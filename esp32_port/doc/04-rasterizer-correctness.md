# Correctness defects and how they were found

This document is a case study of the nine correctness defects that had to be found and
fixed to get Driver 2's PSX primitive stream rendering correctly through a software
rasterizer on an ESP32-S3. It is not a tour of the rasterizer's design; it is a record of
what went wrong, what it looked like on a 320x240 panel, how each one was cornered without
a debugger, and what the actual repair was.

Eight of the nine were found by looking at the panel or at an offline replica of the
arithmetic. The ninth — the last one below — was found by *reading* the code during a
review, after months of running, because every diagnostic instrument in this document
was structurally blind to it. That is the most uncomfortable lesson here and it gets its
own section.

All of the code discussed lives in these places:

| Component | Path |
| --- | --- |
| Software rasterizer | `E:/Hardware/Driver2/REDRIVER2/src_rebuild/PsyCross/src/gpu/PsyX_SoftRas.cpp` |
| Primitive walk / draw-env decode | `E:/Hardware/Driver2/REDRIVER2/src_rebuild/PsyCross/src/gpu/PsyX_GPU.cpp` |
| ESP32 VRAM helpers | `E:/Hardware/Driver2/REDRIVER2/esp32_port/main/esp_stubs.cpp` |
| ESP32 serial dumps / present | `E:/Hardware/Driver2/REDRIVER2/esp32_port/main/esp_platform.cpp` |
| ESP32 panel conversion | `E:/Hardware/Driver2/REDRIVER2/esp32_port/main/display_esplcd.c` |

## Summary

| # | Defect | Symptom on the panel | Fixed in |
| --- | --- | --- | --- |
| 1 | Row-start attribute computed by an out-of-range `float`→`int` cast | Whole primitives sampling a constant wrong texel; noise on thin geometry | `4f049c6` |
| 2 | Gradient conversion could saturate the same way | (not observed; defensive) | `4f049c6` |
| 3 | `modulate()` clamped only the upper bound | Bright white speckle on gouraud-textured surfaces | `4f049c6` |
| 4 | Triangle area limit rejected half of a screen-spanning quad | Diagonal hole across sky/ground | `4f049c6` |
| 5 | `SPRT` (0x64) aliased onto `TILE` (0x60) under `& 0xF8` | Wrong prim length; the OT walk desynchronised | pre-`bfbed95` |
| 6 | Missing 8-bit texcoord wrap + wrong texture-window formula | Garbled menu/HUD text | pre-`bfbed95` |
| 7 | `GR_CopyVRAM` dereferenced `src` unconditionally | Crash on the first `MoveImage` | `fa95b9a6` (REDRIVER2) |
| 8 | Minification filter dropped the semi-transparency bit | The car's shadow became an opaque black rectangle | `feaaeb2` |
| 9 | 555→565 panel conversion left red in the low bits | **Red and blue exchanged in every frame** — orange sky, blue asphalt | current tree (`display_esplcd.c`) |

### A note on provenance

Defects 1-4 and 8 are visible as real before/after hunks in the PsyCross git history
(`git log --format=%B` in `src_rebuild/PsyCross`), and this document quotes those hunks
verbatim. Defect 7 is a real hunk in the REDRIVER2 history.

Defects 5 and 6 were found and fixed **before** the rasterizer's first commit
(`bfbed95`, "Software rasterizer for the PSX primitive stream"), so only their *fixed*
form is in the tree; the commit message lists them as delivered features rather than
fixes. Where this document shows the broken code for those two, it is reconstructed from
the commit message and from what the surrounding code makes possible — that is called out
explicitly in each section. The same caveat applies to defect 8's broken form, which was
caught during the development of the filter and is documented in `feaaeb2`'s message
rather than as a separate commit.

Defect 9 is different again: it was found by review long after the fact, and the repair
is in the current tree (`esp32_port/main/display_esplcd.c`, the `psx555toPanel` helper
and the comment block above it). The broken form is quoted from that comment, which
records what the code used to do and why it was wrong.

---

## The instruments: debugging a rasterizer with no debugger

The board has no JTAG probe attached and no framebuffer you can inspect from a host. Every
diagnosis below rests on four instruments that were built specifically to make the failures
observable.

**1. Serial dumps of what is actually in VRAM.** `SoftRas_PresentESP` in
`esp32_port/main/esp_platform.cpp:88` intercepts four single-character commands typed on
the USB-serial console and prints VRAM as hex, throttled with `vTaskDelay(1)` so the
console can drain:

| Key | What it prints | Code |
| --- | --- | --- |
| `p` | The display rect, downscaled to 160x120 RGB332 | `esp_platform.cpp:129` |
| `v` | The whole 1024x512 VRAM at half scale | `esp_platform.cpp:102` |
| `r` | **Raw 16-bit VRAM words** of one texture page, 64 words x 256 rows | `esp_platform.cpp:105` |
| `t` | One texture page **decoded exactly as the rasterizer samples it** | `esp_platform.cpp:123` |

The distinction between `r` and `t` is the whole point: `r` is the data, `t` is the data
plus the sampling logic. Being able to compare them separately is what made defect 1
findable.

**2. A decoder that shares the rasterizer's own code path.**
`SoftRas_DebugDecodeTPage` (`PsyX_SoftRas.cpp:755`) saves the render state, points it at a
given tpage/CLUT, and renders a 128x128 window through the *real* `fetchTexel<0>`:

```c
extern "C" void SoftRas_DebugDecodeTPage(int baseX, int baseY, int clutX, int clutY,
                                         int u0, int v0, unsigned short* out)
{
    const SRState saved = st;
    st.texBaseX = baseX; st.texBaseY = baseY;
    st.texPage = &vram[baseY * VRAM_WIDTH + baseX];
    loadClut(((clutY & 0x1FF) << 6) | ((clutX >> 4) & 0x3F));   // also fills clut16
    st.texFmt = 0;
    st.twMaskU = st.twMaskV = ~0;
    st.twOffU = st.twOffV = 0;

    // 1:1 so fine 4bpp detail survives — decimating here would look like noise
    for (int v = 0; v < 128; v++)
        for (int u = 0; u < 128; u++)
            out[v * 128 + u] = fetchTexel<0>(st, u0 + u, v0 + v);

    st = saved;
}
```

It deliberately does *not* reimplement sampling. If the texture fetch is wrong, this dump
is wrong in the same way, and that is diagnostic information.

**3. Offline replicas of the arithmetic.** Five small host programs (`audit.cpp` through
`audit5.cpp`) re-implement `rasterTri`'s attribute pipeline byte-for-byte alongside an
exact reference computed in `long long` / `__int128`, then sweep hundreds of thousands of
triangles and *attribute* every disagreement to a specific cast or accumulator. This is
what turned "the textures look wrong" into "217 triangles are wrong and all 217 are the
`uRow` cast". Their output is quoted in the sections below.

**4. An offline decoder for the original game data** (`lev.py`): it walks
`HAVANA.LEV`, finds `LUMP_TEXTUREINFO` (34), RLE-decompresses a texture page and renders
it with palette 0, entirely independently of any port code.

**The blind spot these four share.** Every one of them reads `vram[]` — the emulated PSX
framebuffer — and reconstructs an image from it on the host. None of them observes what
the *panel* was actually sent. The conversion from PSX BGR555 to the ST7789's RGB565
sits downstream of all four, so a defect introduced there is invisible to the entire
instrument set by construction. That is exactly where defect 9 lived, undetected, for
months. Worth stating up front: a debug pipeline that taps the data *before* the last
transform can only ever validate everything except the last transform.

---

## Bisecting the pipeline: is it the data or the sampling?

Before any of the arithmetic defects could be diagnosed, one question had to be settled:
when a textured surface renders as noise on the panel, is the texture *data* in VRAM
corrupt — a bad DMA, a bad flash read, a bad `LoadImage`, a PSRAM cache coherency problem —
or is the data fine and the *sampling* wrong?

These are very different bugs and it is easy to burn a day on the wrong one. The bisection
was done as follows.

**Step 1 — get the raw words off the device.** The `r` command prints the raw contents of
the scenery texture page, VRAM x=448, y=0, 64 words wide by 256 rows (64 words x 4 texels
per word = 256 texels), as plain hex:

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
```

The captured log begins:

```
#RAW 448 0 64 256
88888888ce5ff5f488888888888888888888d888888d888888888888888888884f5ff5ec88888888
8888f88888f74ff8888f8888888888888888d68888d68888888888888888f8888ff47f88888f8888
```

**Step 2 — decode the same page from the shipped game file, on the host.** `lev.py`
opens `data/DRIVER2DEMO/LEVELS/HAVANA.LEV`, reads the four `(offset, size)` city lumps
from the header, walks `DATA1` to find lump 34, extracts the permanent-texture list, then
for each entry reads the palette count, decompresses the page and expands it as 4bpp:

```python
def decomp(src, off):
    out = bytearray(0x8000); ptr = 0x7fff
    while ptr >= 0:
        pix = src[off]; off += 1
        if pix & 0x80:
            b = src[off]; off += 1
            for _ in range(258 - pix):
                if ptr < 0: break
                out[ptr] = b; ptr -= 1
        else:
            for _ in range(pix + 1):
                if ptr < 0: break
                out[ptr] = src[off]; off += 1; ptr -= 1
    return bytes(out)
```

Running it confirms the container parse is right before anything else is trusted — the
sum of the padded per-page sizes must equal the declared lump size:

```
tpage  0  size  32692  npalettes 24  decompressed 32768
tpage  1  size  30268  npalettes 21  decompressed 32768
...
total tpage bytes 378880 lump size 378880
```

Every page decompresses to exactly 32768 bytes = 256 x 256 texels at 4bpp, and the total
matches the lump exactly. The decoder is correct.

**Step 3 — render both to PNG and compare.** The device's raw hex was rendered with the
same palette and the same 4bpp unpack as the host reference. The two files:

```
f853e317514462f7e34a91b7f79225b0 *ref_pal0.png    (rendered from HAVANA.LEV on the host)
f853e317514462f7e34a91b7f79225b0 *esp_page.png    (rendered from the board's VRAM dump)
```

Identical MD5. **Pixel-identical.** The texture bytes sitting in the ESP32's PSRAM VRAM
were exactly the bytes in the retail level file, after flash mapping, after the spooler,
after `LoadImage`, after RLE decompression on-device.

That single comparison eliminated an entire class of hypotheses — bad flash reads, a
cache-coherency problem between the two cores and PSRAM, a broken `LoadImage`, a wrong
tpage base — and left exactly one place for the bug to be: the code that turns
`(u, v)` into an address. From there, defects 1, 2 and 6 were all in scope, and the
investigation moved to the arithmetic.

---

## Defect 1 — the row-start attribute was an out-of-range float cast

This is the important one. Everything else in this document is a normal bug; this one is a
lesson about which kind of integer arithmetic is safe to let overflow.

### Symptom

Textured geometry sampling a *constant wrong offset* into the texture page — not jitter,
not a gradient error, but the whole primitive shifted onto unrelated texels, producing
noise or a flat wash of the wrong colour. It was worst on thin, elongated geometry:
railings, kerbs, distant road strips, the near-edge-on faces of buildings. Fat triangles
looked fine.

That asymmetry — thin bad, fat fine — is the clue, and it is the opposite of what a
precision problem looks like.

### The code

```c
uRow = (int)(((float)((w0row - bias0) * v0.u
                    + (w1row - bias1) * v1.u
                    + (w2row - bias2) * v2.u)) * invArea * 65536.0f) + 32768;
```

Read what this computes. `w0row`, `w1row`, `w2row` are the three edge functions evaluated
at `(minX, minY)`. That is the corner of the triangle's bounding box **after clipping**
(`PsyX_SoftRas.cpp:322-325`). The weighted sum divided by `area` is the barycentric
interpolation of `u` — but evaluated at a point that, for a sliver, is nowhere near the
triangle. It is a linear *extrapolation*, and extrapolation along a steep gradient blows
up fast.

### Diagnosis

`audit.cpp` replicates this exact expression next to an exact `__int128` reference and
reports, per triangle, whether each of the three int32 conversions went out of range and
what the worst per-covered-pixel texel error was. A 400,000-triangle randomized sweep,
bucketed by covered-pixel count:

```
cov 1..7       n=54372    worstTexelErr=32722  overflowCases=92
cov 8..63      n=55950    worstTexelErr=32148  overflowCases=65
cov 64..511    n=60698    worstTexelErr=27838  overflowCases=9
cov 512..4k    n=62743    worstTexelErr=1      overflowCases=0
cov 4k..32k    n=46092    worstTexelErr=1      overflowCases=0
cov >32k       n=3725     worstTexelErr=1      overflowCases=0
```

The error is not a few texels. It is ~32700 texels — i.e. a completely unrelated address —
and it only happens on small/thin triangles. `audit2.cpp` then attributes each failure to
a specific cast:

```
triangles with >1 texel u error: 217
  attributable to GRAD (int) cast overflow : 0
  attributable to uRow (int) cast overflow : 217
  attributable to u16 accumulator overflow : 0
  UNEXPLAINED (pure precision)             : 0  worst=0.000 texels
```

Unambiguous. Not the gradients, not the per-pixel accumulator, not floating-point
precision. The row start, every time.

`audit5.cpp` then pins down the mechanism and the threshold:

```
(int)3.0e9  = -2147483648
(int)-3.0e9 = -2147483648
(int)2.5e12 = -2147483648   [INT_MIN=-2147483648]

uRow (int) cast saturates when |u_start| >= 2^31/65536 = 32768 texels
  u_start = u linearly extrapolated to the CLIPPED bbox corner (minX,minY)
  for a sliver of thickness t carrying du=255 across it, |grad u| = 255/t,
  bbox-corner distance D  ->  saturates when 255*D/t >= 32768, i.e. D/t >= 128.5
   bbox diagonal D=400   px -> saturates for thickness t <= 3.11 px
   bbox diagonal D=320   px -> saturates for thickness t <= 2.49 px
   bbox diagonal D=240   px -> saturates for thickness t <= 1.87 px
```

There is the thin/fat asymmetry, quantified: a triangle that crosses the 320-pixel screen
and is thinner than ~2.5 pixels saturates. That is a fair description of a kerb, a
railing, or a road strip near the horizon. The commit message records the same saturation
to `INT_MIN` on the Xtensa device; the host replica above reproduces it on x86 for both
signs.

### Root cause

Two distinct things are wrong, and they compound.

**(a) It is undefined behaviour.** In C and C++, converting a floating-point value to an
integer type when the truncated value cannot be represented is undefined. It is not
"implementation-defined", it is not "wraps": the compiler is entitled to assume it never
happens. In practice the hardware conversion instruction *clamps*, so the result is one
endpoint of the integer range.

**(b) Clamping is the worst possible failure mode here.** The value is a 16.16 fixed-point
attribute that is *about to be used as the seed of a DDA*. Every pixel of every row of the
primitive is derived from it by adding gradients. A clamp replaces the true value with
`INT_MIN`, which bears no arithmetic relationship to the true value at all, so the whole
primitive is offset by a constant garbage amount that never corrects itself. One bad cast
poisons tens of thousands of pixels.

### The fix

Compute the row start in integers, extrapolating from `v0` rather than from a barycentric
weighted sum:

```c
    // Row start: extrapolate from v0 to the centre of pixel (minX,minY), in
    // 16.16, entirely in integers.
    //
    // This replaces a float expression that was undefined behaviour: (minX,minY)
    // is the clipped bounding-box corner, which for a sliver triangle can sit
    // far off the triangle, so the extrapolated value exceeded 2^31 in 16.16 and
    // the cast saturated — making the start value a constant garbage offset that
    // then propagated down every row. Integer arithmetic wraps instead, and the
    // wrap cancels exactly against the per-pixel accumulator, so covered pixels
    // still land on the correct texel.
    #define ROWSTART(a0, dx, dy) (int)((unsigned)((a0) << 16) \
        + (unsigned)(dx) * (unsigned)(minX - v0.x) \
        + (unsigned)(dy) * (unsigned)(minY - v0.y) \
        + (unsigned)((dx) >> 1) + (unsigned)((dy) >> 1))
```

(`PsyX_SoftRas.cpp:367-370`, used at `375-388`.)

### Why letting this overflow is *correct*, not merely tolerable

This is the key insight of the whole exercise, so it is worth stating precisely.

Every arithmetic operation inside `ROWSTART` is on `unsigned int`. C guarantees unsigned
arithmetic is performed **modulo 2^32** — exactly, with no undefined behaviour, no
saturation, no compiler licence to assume it does not happen. So the value stored in
`uRow` is not "approximately the row start, or `INT_MIN` if it overflowed". It is *exactly*
the true row start, reduced modulo 2^32.

Now follow that value into the pixel loop. The span start advances by `duDx * adv`
(`PsyX_SoftRas.cpp:525`), the inner loop adds `duDx` per pixel (`line 610`), and the row
loop adds `duDy` per row (`line 616`). All of those are `int` additions of the same 16.16
quantities. Composing them, the accumulator at any pixel `(x, y)` is:

```
u16(x, y) = (v0.u << 16)
          + duDx * (x - v0.x)
          + duDy * (y - v0.y)
          + duDx/2 + duDy/2                    (all mod 2^32)
```

The intermediate excursion at `(minX, minY)` **cancels**. Whatever multiple of 2^32 was
lost when the row start was reduced is put back by the accumulation, because addition
modulo 2^32 is associative and the whole chain is one linear expression evaluated in the
same ring.

The final step is the one that makes it *useful*. The affine attribute is a convex
combination of the three vertex values at any point *inside* the triangle. Vertex `u` and
`v` are PSX 8-bit texture coordinates, so at a covered pixel the true value lies in
`[0, 255]`, plus DDA truncation drift, which `audit5` bounds:

```
DDA truncation: |duDx| trunc err < 1/65536 texel/step
  over 1024 x-steps : 0.01562 texels
  over 512 y-steps  : 0.00781 texels
```

So at any pixel the coverage test accepts, `|u16| < 256 << 16`, which is nine orders of
magnitude inside the 2^31 range. A value that small has exactly one representative in
`[-2^31, 2^31)`, and that representative *is* the true value. Reading the accumulator as a
signed `int` therefore yields the correct texel.

The garbage exists only at pixels the half-space test rejects — pixels that are never
sampled and never written. The wraparound is not a hazard that was accepted; it is the
mechanism that makes the computation exact.

Contrast the two:

| | Float path | Integer path |
| --- | --- | --- |
| Out-of-range behaviour | Undefined; in practice clamps to an endpoint | Defined: exact modulo 2^32 |
| Relationship to true value | None | Congruent mod 2^32 |
| Recovered by the DDA? | No — constant offset forever | Yes — the wrap cancels exactly |
| Pixels affected | Every pixel of the primitive | None that are covered |

The same commit also replaced the crude `+ 32768` (half a *texel*) with the proper
half-pixel sample offset `duDx/2 + duDy/2` — the value at the *centre* of pixel
`(minX, minY)` rather than at its corner plus half a texel. These coincide only when
`du/dx == 1` and `du/dy == 0`. They diverge most where the texel-per-pixel ratio is
extreme, which on this content is the player car: measured at **11-23 texels per pixel**
on 2-3 pixel polygons, against 1.2-1.6 on the 100-200 pixel ground and building polygons.

### Verification

- The `audit2` attribution run, repeated against the integer form, reports zero triangles
  with >1 texel error and zero unexplained cases.
- `audit3.cpp` searched specifically for large triangles (>200 covered pixels) that the
  *fixed* pipeline still gets wrong. It finds only residual off-by-one texel selections at
  the boundary between two texels — `wrongTexels=31/50289`, `wrongTexels=88/47047`,
  `maxErr=0` — i.e. rounding at texel edges with zero magnitude error, and with
  `ovf(G,R,A)=000` on every one. No overflow-class failures remain.
- On the device, the `t` dump (decoded through the real `fetchTexel`) was compared against
  the `r` dump rendered offline; they agree, closing the loop opened in the bisection
  section.

### Residual trade-off, stated plainly

`ROWSTART` is a macro that captures `minX`, `minY` and `v0` from the enclosing scope. That
is fragile in the ordinary way macros are fragile; it is `#undef`-ed immediately after use
(`line 389`) to limit the blast radius, but it is a wart.

The final `(int)` conversion of an `unsigned` value above `INT_MAX` was
implementation-defined before C++20 and is defined as two's-complement wrapping from C++20
on; GCC has always documented it as modular. This is relied upon.

---

## Defect 2 — the gradient conversion could saturate the same way

### Symptom

None observed. This one is defensive, and it is worth being honest that it was fixed
because the analysis said it *could* fire, not because it was seen.

### Diagnosis

Having established the mechanism in defect 1, the obvious question is whether the other
`float`→`int` conversion in the same function can hit it. The gradient macro was:

```c
    #define GRAD(p0, p1, p2) \
        dDx = (int)(((float)(A12 * (p0) + A20 * (p1) + A01 * (p2))) * invArea * 65536.0f), \
        dDy = (int)(((float)(B12 * (p0) + B20 * (p1) + B01 * (p2))) * invArea * 65536.0f)
```

Two separate things could go wrong. `audit5.cpp` checks the first — whether the plain-`int`
numerator itself can overflow:

```
GRAD numerator worst case |A|max=65534 (short coords), attr<=255:
  3 * 65534 * 255 = 50133510   vs INT_MAX 2147483647  -> safe (23.4x margin)
  PSX 11-bit coords (|A|<=2047): 3*2047*255 = 1565955 -> safe (1371x margin)
```

Safe, even under the pessimistic assumption that a vertex coordinate uses the full range of
`VERTTYPE`. That type is `short` in this build: `USE_PGXP=0` (set in
`esp32_port/main/CMakeLists.txt:71`) takes the `#else` arm of `pgxp_defs.h`, and the only
`VERTTYPE` definition in that arm is `typedef short VERTTYPE;` at
`PsyCross/include/PsyX/common/pgxp_defs.h:71`. (The two definitions at lines 17 and 20 of
the same header are inside `#if USE_PGXP` and are not reachable here — worth checking
rather than assuming, because the C++ one of that pair is `half`, not `short`.)

The second is the cast itself. `invArea` is `1.0f / area`, and `area` is an integer that
can legitimately be 1 for a degenerate sliver. With an 11-bit coordinate range the
numerator can reach ~3.1e6, so `numerator * invArea` can reach ~3.1e6 texels per pixel
step, and 16.16 saturates past 32768. The same undefined cast, the same clamp.

`audit2`'s attribution found `castOverflowGrad = 0` across the sweep — this never actually
fired on realistic geometry. It fires only on triangles of area 1-2, which cover one or two
pixels.

### The fix

```c
// attribute-per-pixel gradient -> 16.16, guarded against an out-of-range cast
static inline int gradToFixed(float g)
{
    if (g >  256.0f) return  (256 << 16);
    if (g < -256.0f) return -(256 << 16);
    return (int)(g * 65536.0f);
}
```

(`PsyX_SoftRas.cpp:289-295`.) Note the clamp is applied to `g` in *texels per pixel*,
before the multiply by 65536, so the comparison itself can never be against an
already-overflowed value.

The point is not that 256 texels per pixel is a meaningful gradient — it is not; any such
triangle is sampling noise regardless. The point is that the behaviour becomes *defined and
bounded*: a covered pixel gets some wrong texel from within the page, instead of the
program executing undefined behaviour whose consequences the optimiser is free to
propagate.

### Verification

The clamp is unreachable on measured content (the 400k-triangle sweep never triggers it),
so verification here is by construction and by the numeric bound above rather than by
observation. Stated as such.

---

## Defect 3 — `modulate()` clamped only the upper bound

### Symptom

Isolated pixels of full-brightness white or saturated colour scattered over
gouraud-shaded textured surfaces — the classic "speckle". Sparse, moving, and clustered
around the darker parts of a surface.

### The code

```c
    int r = ((texel & 31) * r8) >> 7;
    int g = (((texel >> 5) & 31) * g8) >> 7;
    int b = (((texel >> 10) & 31) * b8) >> 7;
    if (r > 31) r = 31;
    if (g > 31) g = 31;
    if (b > 31) b = 31;
    return (unsigned short)(r | (g << 5) | (b << 10) | (texel & 0x8000));
```

### Root cause

`r8`, `g8`, `b8` come from the gouraud accumulators, `r16 >> 16` and friends
(`PsyX_SoftRas.cpp:598`). Those are affine-interpolated the same way `u` and `v` are, and
for the same reasons they can land fractionally below the range spanned by the three vertex
colours: gradients are truncated to 16.16, so the accumulated value can undershoot by a
fraction of a unit. Driver 2 emits vertex colours of 0 routinely (fog, shadowed faces,
night lighting), so "one below the minimum" means **-1**.

Then look at what a single negative channel does. `>>` on a negative `int` is an arithmetic
shift, rounding toward negative infinity, so a value of `-1` stays `-1` — which is
`0xFFFFFFFF`. That is not one dark pixel. It is OR-ed into the result alongside `g << 5`
and `b << 10`, so it sets every bit of the 16-bit pixel: all three 5-bit channels go to
31, *and* bit 15 (the semi-transparency flag) is set. One slightly-negative red channel
produces a fully white, semi-transparent pixel.

The failure is wildly out of proportion to the error that caused it. An arithmetic
undershoot of one part in 65536 becomes a maximum-magnitude visual artefact, which is
exactly why it reads as speckle rather than as slight shading error.

Before defect 1 was fixed there was a second, larger path to the same place: a poisoned
`rRow` made the colour accumulator arbitrary, so negative values were not marginal at all.

### The fix

```c
    // Clamp BOTH ends: the gouraud accumulators can run negative outside the
    // triangle's colour range, and a negative channel OR-ed into the pixel
    // smears sign bits across all three channels — random bright speckle.
    if (r > 31) r = 31; else if (r < 0) r = 0;
    if (g > 31) g = 31; else if (g < 0) g = 0;
    if (b > 31) b = 31; else if (b < 0) b = 0;
```

(`PsyX_SoftRas.cpp:205-210`.) The `else if` form is deliberate: the two branches are
mutually exclusive, so this is one predicated select per channel rather than two.

### Verification

The speckle disappeared from the panel. This one is a visual verification and there is no
numeric bound to quote — but the mechanism is exhaustively checkable by inspection, since
the only inputs are the three channel products and the only failure is the sign bit.

Note that the same `modulate()` is now also used to pre-compute the 16-entry modulated
palette once per triangle (`PsyX_SoftRas.cpp:441-444`), where its inputs are `v0.r/g/b`,
always in `[0, 255]`. That path could never have produced the bug, which is part of why it
only appeared on gouraud surfaces.

---

## Defect 4 — the area limit ate half of a screen-spanning quad

### Symptom

A hard-edged **diagonal hole** across large surfaces — most visible in the sky and on the
ground plane. Not a flicker, not a z-fighting seam: a clean triangular wedge of background
showing through a surface that should be solid, with the missing region bounded by the
quad's own diagonal.

The shape is the diagnosis. Driver 2's `POLY_F4`/`POLY_FT4` quads are split into two
triangles along the 1-2 diagonal at `PsyX_SoftRas.cpp:820-821`:

```c
            drawTri(a, b, c, true, false, semi);
            drawTri(b, d, c, true, false, semi);
```

A hole bounded exactly by that diagonal means one of the two calls drew and the other did
not. Nothing about clipping, ordering or blending produces that silhouette; only a
per-triangle early return does.

### The code

```c
    // PSX size limits: reject absurd triangles (protects against garbage prims)
    if (area > (long long)1024 * 512 * 2)
        return;
```

### Root cause

The limit is 1,048,576. That number was chosen as "twice the area of VRAM" on the reasoning
that nothing legitimate can be bigger. It is wrong, because primitive coordinates are in
*pre-clip* screen space and routinely extend well outside the 320x240 viewport — a ground
quad drawn at the horizon can span several thousand units in x.

Worse, the two halves of a quad do not have equal area unless the quad is a parallelogram.
`audit.cpp` shows both facts directly:

```
=== C: full-screen road-like quad halves ===
road quad tri A (320x240, uv 0..255)  area=76241  cov=38399 ... drop=0
road quad tri B                       area=76241  cov=37842 ... drop=0

=== D: off-screen-large polys (short VERTTYPE allows +-32767) ===
sky quad -2000..2000 x                area=4000000  cov=0 ... drop=2
huge tri (area over cap)              area=2094081  cov=0 ... drop=2
```

`drop=2` is the area-cap rejection. A quad whose two halves straddle 1,048,576 — say
900k and 1.2M — loses exactly one triangle, and what remains is a hole bounded by the
diagonal.

### The fix

```c
    // Reject only genuinely absurd triangles. The old 1024*512*2 limit threw
    // away one half of a legitimately screen-spanning quad (sky, ground),
    // leaving a diagonal hole, since the two halves can straddle the bound.
    if (area > 64 * 1024 * 1024)
        return;
```

(`PsyX_SoftRas.cpp:310-314`.)

67,108,864 is chosen to sit above anything a legal primitive can produce while still
catching a corrupt packet. The bound comes from the same reasoning that let the edge
functions drop from `long long` to `int` in the same commit:

```c
// 32-bit is sufficient and much cheaper than 64-bit on Xtensa: PSX primitive
// coords are 11-bit signed and the draw offset adds at most 2047, so every
// term here stays under 2^25.
static inline int edgeFn(const SRVert& a, const SRVert& b, int px, int py)
{
    return (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
}
```

That claim is checkable. Driver 2 obtains its screen coordinates from `gte_stsxy`, and the
GTE saturates SX2/SY2 to 11 bits signed — `Lm_G1` / `Lm_G2` in
`PsyCross/src/gte/PsyX_GTE.cpp:216-241` clamp to `[-0x400, 0x3ff]` and raise the overflow
flag. With the draw offset (`ofs`, `short`) adding at most 2047, each difference is bounded
by ~4094, each product by ~1.7e7 < 2^25, and the difference of two such terms stays
comfortably inside `int`. So `area` can never legitimately exceed ~3.3e7, and 6.7e7 is a
guard that legal geometry cannot trip.

### Verification

The holes went away in the sky and on the ground. The `audit` sweep reports
`dropped_by_area_cap = 0` in every bbox-area bucket once the bound is raised (`audit2`
output), i.e. no realistic triangle is rejected at all.

### Trade-off

The guard is now essentially inert for correct input. If a corrupt packet produces
coordinates outside the GTE's 11-bit range — which the packet's `short` fields permit
even though the GTE cannot emit them — the `int` edge function could overflow and the area
test would be evaluating a meaningless number. That is a knowingly accepted risk taken in
exchange for the inner loop, and it is why the OT walk still carries its own
`tagLength > 32` sanity check upstream (`PsyX_GPU.cpp:874`).

---

## Defect 5 — `SPRT` collided with `TILE` under a `& 0xF8` mask

*(Fixed before the first rasterizer commit; the broken mask below is reconstructed from
the commit message. The shipped decode and the length table it must produce are both in
the tree and are quoted verbatim.)*

### Symptom

Not a rendering artefact — a *structural* failure. Sprites drew as untextured rectangles,
and then everything after them in the same OT tag was wrong: primitives drawn with the
wrong type, garbage geometry, and the console printing

```
did not output valid primitive or ptag length is not valid (diff=%d)
```

from `PsyX_GPU.cpp:890`.

### Diagnosis

That message is the giveaway, and it is worth explaining why. PsyX's ordering-table walk
supports multiple primitives packed into one tag. `SR_WalkOT` (`PsyX_GPU.cpp:865`) uses the
tag's own length to find the end of the packet, then advances *by the length each decoder
returns*:

```c
                uintptr_t currentPacket = basePacket;
                const uintptr_t endPacket = basePacket + (tagLength + P_LEN) * sizeof(u_int);
                int primLength = 0;
                while (currentPacket < endPacket)
                {
                    primLength = ParsePrimitive(reinterpret_cast<P_TAG*>(currentPacket));
                    currentPacket += (primLength + P_LEN) * sizeof(u_int);
                }

                if (currentPacket != endPacket)
                {
                    eprinterr("did not output valid primitive or ptag length is not valid (diff=%d)\n", ...);
                }
```

So the return value of `SoftRas_Primitive` is not advisory. It is the stride of the walk.
Return the wrong number once and the pointer lands mid-primitive; from there every
subsequent decode reads a shifted word as a `code` byte and the rest of the tag is
nonsense. `diff` in the message is exactly how many bytes the walk overshot or undershot,
which is how the specific primitive was identified.

### Root cause

The PSX TILE/SPRT opcode space, from the setter macros in
`PsyCross/include/psx/libgpu.h:301-308`:

| Macro | Code | Length (longs) |
| --- | --- | --- |
| `setTile` | `0x60` | 3 |
| `setSprt` | `0x64` | 4 |
| `setTile1` | `0x68` | 2 |
| `setTile8` | `0x70` | 2 |
| `setSprt8` | `0x74` | 3 |
| `setTile16` | `0x78` | 2 |
| `setSprt16` | `0x7c` | 3 |

Masking with `& 0xF8` clears bit 2 — which is precisely the *textured* bit that separates a
sprite from a tile:

```
0x64 & 0xF8 == 0x60     SPRT      -> decoded as TILE      (length 4 reported as 3)
0x74 & 0xF8 == 0x70     SPRT_8    -> decoded as TILE_8    (length 3 reported as 2)
0x7c & 0xF8 == 0x78     SPRT_16   -> decoded as TILE_16   (length 3 reported as 2)
```

Every sprite form aliases onto a tile form, drawing as a flat rectangle *and* under-reporting
its length by exactly one long. The mask was presumably chosen to fold the semi-transparency
bit (0x02) and the shade-texture bit (0x01) away, and it took bit 2 with them.

The reference GL backend never had this problem because it masks with `& 0xFD` —
clearing only the semi-transparency bit — and then switches on the full code
(`PsyX_GPU.cpp:1341`).

### The fix

Decode the two orthogonal fields instead of pattern-matching whole opcodes:

```c
    case 0x60:
    case 0x70:  // TILE / SPRT family
    {
        loadTpage(activeDrawEnv.tpage);

        // bit 2 (0x04) = textured (SPRT family); bits 3-4 (0x18) = size:
        // 0x00 variable, 0x08 1x1, 0x10 8x8, 0x18 16x16
        const int sizeCode = code & 0x18;
        const bool isSprite = (code & 0x04) != 0;

        if (isSprite)
        {
            if (sizeCode == 0x00)
            {
                SPRT* p = (SPRT*)tag;
                loadClut(p->clut);
                drawSprite(p->x0, p->y0, p->w, p->h, p->u0, p->v0, p->r0, p->g0, p->b0, semi);
                return 4;
            }
            // fixed-size sprites share the SPRT_8/16 layout
            SPRT_8* p = (SPRT_8*)tag;
            const int side = (sizeCode == 0x08) ? 1 : (sizeCode == 0x10) ? 8 : 16;
            loadClut(p->clut);
            drawSprite(p->x0, p->y0, side, side, p->u0, p->v0, p->r0, p->g0, p->b0, semi);
            return 3;
        }
        ...
```

(`PsyX_SoftRas.cpp:931-969`.) The outer `switch` still keys on `code & 0xF0`, so both
`0x60` and `0x70` arrive at the same arm; within it, bit 2 selects sprite-vs-tile and bits
3-4 select the size. Working the table through this decode reproduces all seven codes and
all seven lengths correctly.

### Verification

The `did not output valid primitive` message stopped appearing, which is a strict check:
the walk must land exactly on `endPacket` for every tag in the frame, so any remaining
length error anywhere in the primitive set would still report. Sprites also started drawing
textured, confirming the type dispatch and not just the length.

---

## Defect 6 — missing texcoord wrap and a wrong texture-window formula

*(Also fixed before the first commit; the broken forms are reconstructed. The trap that
caused it, and the shipped formula, are both verifiable in the tree.)*

### Symptom

Garbled menu and HUD text: glyphs showing fragments of unrelated artwork, or bleeding
into each other, while 3D scenery looked broadly right. Wide sprites were worse than
narrow ones.

### Root cause (a): the 8-bit wrap

PSX texture coordinates are 8-bit and wrap **inside** the 256x256 texture page. `drawSprite`
walks the sprite with raw coordinates:

```c
                case 0:  texel = fetchTexel<0>(st, u0 + x, v0 + y); break;
```

(`PsyX_SoftRas.cpp:675`.) For a sprite whose `u0` is near the right edge of the page,
`u0 + x` exceeds 255 partway across. Without the wrap, the 4bpp fetch computes
`texBaseX + (u >> 2)` and simply keeps walking into the *next texture page* in VRAM — which
in Driver 2 holds different artwork with a different palette. The wider the sprite, the more
of it comes from the wrong page. Font glyph strips are exactly the wide sprites that live
near page edges, which is why text was the visible casualty.

### Root cause (b): the texture window

This one is a genuine trap in PsyX rather than a careless mask, and it deserves the detail.

`DRAWENV.tw` is a `RECT16`. When the *game* fills a `DRAWENV`, `tw` is in **pixels** — the
encoder in `PsyCross/src/psx/LIBGPU.C:352` converts it to the hardware's 5-bit fields:

```c
	dr_env->code[3] = 32 * (((256 - env->tw.h) >> 3) & 0x1F) | ((256 - env->tw.w) >> 3) & 0x1F
	                | (((env->tw.y >> 3) & 0x1F) << 15) | (((env->tw.x >> 3) & 0x1F) << 10) | 0xE2000000;
```

But when PsyX *parses* an `0xE2` (DR_TWIN) packet out of the stream, it writes the raw
5-bit fields straight back into the same struct members:

```c
		case 0x2:
		{
			// DR_TWIN
			activeDrawEnv.tw.w = (code & 0x1F);
			activeDrawEnv.tw.h = ((code >> 5) & 0x1F);
			activeDrawEnv.tw.x = ((code >> 10) & 0x1F);
			activeDrawEnv.tw.y = ((code >> 15) & 0x1F);
			break;
		}
```

(`PsyX_GPU.cpp:1505-1513`.) So `activeDrawEnv.tw.w` means *window width in pixels* if you
read the `DRAWENV` documentation, and *5-bit AND-mask* if you read what the packet parser
actually stores. Any formula written against the first reading is wrong.

It is wrong in the most destructive possible way for this game. Driver 2 never calls
`SetTexWindow` or `SetDrawMode` with a window; the only `0xE2` packets it emits come from
`PutDrawEnv` with the default `tw = {0,0,0,0}`, which encodes to mask fields of
`((256 - 0) >> 3) & 0x1F == 0`. So after parsing, `tw.w == tw.h == 0` — and a formula that
reads that as "a window 0 pixels wide" collapses every texture coordinate to zero (or
divides by zero) instead of reducing to the identity.

### The fix

Apply the hardware formula, on the raw fields, precomputed once per primitive:

```c
    // Texture window: PsyX stores the RAW 5-bit GPU packet fields
    // (tw.w/h = MaskX/Y, tw.x/y = OffsetX/Y), so apply the hardware formula
    //   texcoord = (texcoord AND NOT(Mask*8)) OR ((Offset AND Mask)*8)
    // With Mask == 0 this reduces to "no window", so no special case needed.
    // The mandatory 8-bit wrap of PSX texture coords is folded into the window
    // masks (offsets are <= 248, so the OR can never carry past 255). That
    // removes two ANDs from every texel fetch.
    st.twMaskU = ~(activeDrawEnv.tw.w * 8) & 0xFF;
    st.twOffU  = (activeDrawEnv.tw.x & activeDrawEnv.tw.w) * 8;
    st.twMaskV = ~(activeDrawEnv.tw.h * 8) & 0xFF;
    st.twOffV  = (activeDrawEnv.tw.y & activeDrawEnv.tw.h) * 8;
```

(`PsyX_SoftRas.cpp:131-141`), consumed by two instructions per axis in the fetch:

```c
    u = (u & s.twMaskU) | s.twOffU;
    v = (v & s.twMaskV) | s.twOffV;
```

Three things are worth noticing:

1. **`Mask == 0` is the identity.** `~(0 * 8) & 0xFF == 0xFF` and the offset is
   `(x & 0) * 8 == 0`, so the expression degenerates to `u & 0xFF`. No special case, no
   branch, and the common Driver 2 configuration is handled by the same code as any other.
2. **The mandatory 8-bit wrap is folded into the same mask** (the `& 0xFF` above), so
   root cause (a) is fixed by construction rather than by a separate AND. This was a later
   optimisation (`4f049c6`) that also removed two ANDs from every texel fetch, and it is
   safe because the offset term is at most `31 * 8 == 248`, so the OR can never carry past
   255.
3. **The formula is applied to the coordinate, not to the address.** That is what makes it
   correct for the 4bpp case where `u` is a texel index but the VRAM cell holds four of
   them.

### Verification

Menu and HUD text became legible. Beyond the visual check, the `t` serial dump exercises
this exact code path (`SoftRas_DebugDecodeTPage` sets `twMaskU = twMaskV = ~0` and
`twOff* = 0` to isolate it), and the resulting page was compared against the `lev.py`
decode of `HAVANA.LEV` as described in the bisection section.

---

## Defect 7 — `GR_CopyVRAM` dereferenced a NULL source

### Symptom

A hard fault (`LoadProhibited` on address 0) during the boot sequence, before gameplay was
reached. Reproducible and immediate.

### Diagnosis

Straightforward from the backtrace, but the *reason* it is a defect worth documenting is
the API contract it violates, which is not stated anywhere in a header.

### The code

The ESP32 port supplies its own `GR_CopyVRAM` because there is no GL backend
(`esp32_port/main/esp_stubs.cpp`). The first version was:

```c
void GR_CopyVRAM(unsigned short* src, int x, int y, int w, int h, int dst_x, int dst_y)
{
    extern unsigned short vram[VRAM_WIDTH * VRAM_HEIGHT];
    (void)x; (void)y;
    for (int yy = 0; yy < h; yy++) {
        int dy = (dst_y + yy) & (VRAM_HEIGHT - 1);
        for (int xx = 0; xx < w; xx++)
            vram[dy * VRAM_WIDTH + ((dst_x + xx) & (VRAM_WIDTH - 1))] = src[yy * w + xx];
    }
}
```

Note the `(void)x; (void)y;` — the author (me) concluded the source rect was unused because
`LoadImage` passes a linear buffer. That conclusion is exactly backwards for the other
caller.

### Root cause

`GR_CopyVRAM` is overloaded on `src == NULL`. From `PsyCross/src/psx/LIBGPU.C:131`:

```c
int MoveImage(RECT16* rect, int x, int y)
{
	GR_CopyVRAM(NULL, rect->x, rect->y, rect->w, rect->h, x, y);
	return 0;
}
```

A NULL `src` means *VRAM to VRAM*, and the source rectangle is carried in the `x`/`y`
arguments that the port had explicitly discarded. The reference implementation in
`PsyCross/src/render/PsyX_render.cpp:1652-1665` documents the convention only by doing it:

```c
	if (!src)
	{
		framebuffer_need_update = 1;
		src = vram;
		stride = VRAM_WIDTH;
	}
	src += x + y * stride;
```

Driver 2 reaches this through a `DR_MOVE` packet. `SetDrawMove` (`LIBGPU.C:373-385`)
stamps the tag with code byte `0x01` and puts the GPU's `0x80` "copy rectangle,
VRAM to VRAM" command in the next word, so `ParsePrimitive` dispatches it in the
`primType == 0x00` arm, sub-type `0x1` (`PsyX_GPU.cpp:1610-1640`). In gameplay the caller
is `sky.c`, which blits a rectangle of the framebuffer in order to sample the sun.

### The fix

```c
// src == NULL means a VRAM->VRAM move: MoveImage() passes the source rect in
// x/y (LIBGPU.C). Otherwise src is a linear w*h source image (LoadImage).
void GR_CopyVRAM(unsigned short* src, int x, int y, int w, int h, int dst_x, int dst_y)
{
    extern unsigned short vram[VRAM_WIDTH * VRAM_HEIGHT];
    static unsigned short row[VRAM_WIDTH];   // staging: the rects may overlap

    if (w > VRAM_WIDTH) w = VRAM_WIDTH;

    // when moving down over itself, go bottom-up so rows are read before write
    const int step = (src == NULL && dst_y > y) ? -1 : 1;
    const int first = (step < 0) ? h - 1 : 0;

    for (int i = 0; i < h; i++) {
        const int yy = first + i * step;
        const unsigned short* s;

        if (src) {
            s = src + yy * w;
        } else {
            const int sy = (y + yy) & (VRAM_HEIGHT - 1);
            for (int xx = 0; xx < w; xx++)
                row[xx] = vram[sy * VRAM_WIDTH + ((x + xx) & (VRAM_WIDTH - 1))];
            s = row;
        }

        const int dy = (dst_y + yy) & (VRAM_HEIGHT - 1);
        for (int xx = 0; xx < w; xx++)
            vram[dy * VRAM_WIDTH + ((dst_x + xx) & (VRAM_WIDTH - 1))] = s[xx];
    }
}
```

(`esp_stubs.cpp:83-113`.) Two things were added beyond the null check, because a
VRAM-to-VRAM move can overlap its own source:

- A one-row staging buffer, so a horizontal shift within a row cannot read bytes it has
  already overwritten.
- Row iteration direction chosen from the sign of `dst_y - y`, so a downward move reads each
  row before that row is written.

Both wrap with `& (VRAM_HEIGHT - 1)` / `& (VRAM_WIDTH - 1)`, matching the PSX's VRAM
addressing.

### Verification

Boot proceeded past the fault and reached gameplay. This defect also has a second life in
the dual-core work: because the sun-sampling `MoveImage` *reads* pixels both cores are
writing, ignores the clip rect, and stages through a single shared row buffer, it is one of
the two primitives that must go through a full rendezvous. That is why
`PsyX_GPU.cpp:1628-1638` wraps it:

```c
#ifdef ESP32_PORT
			// VRAM-to-VRAM blit. sky.c samples the sun straight out of the
			// framebuffer, so this READS pixels both cores are writing, ignores
			// the clip rect entirely, and stages through a single shared row
			// buffer. Full rendezvous; core 0 performs it.
			if (SR_BarrierEnter())
				MoveImage(&rect, x, y);
			SR_BarrierExit();
#else
			MoveImage(&rect, x, y);
#endif
```

The `static unsigned short row[VRAM_WIDTH]` staging buffer added by the fix is precisely the
shared state that made the rendezvous necessary — a fix in one dimension creating a
constraint in another, which is worth naming rather than hiding.

---

## Defect 8 — the minification filter dropped the semi-transparency bit

### Symptom

The player car's shadow rendered as a solid, opaque black rectangle sitting on the road —
sharply worse than no shadow at all. It appeared the moment the texture minification filter
was enabled and disappeared when it was toggled off with `f` on the serial console.

### Diagnosis

The `f` toggle is itself the diagnostic instrument here. `g_srFilter`
(`PsyX_SoftRas.cpp:96`) is a plain runtime flag read at `line 459`, exposed over serial by
`esp_platform.cpp` (REDRIVER2 commit `208c6e7e`). Being able to flip a suspect code path on
and off on the running device, in the same scene, without reflashing, reduced the search
space to the ~40 lines the filter added.

### Root cause

The filter averages `taps` samples of the pre-modulated palette by unpacking and
re-accumulating the three 5-bit colour channels:

```c
                        int ar = 0, ag = 0, ab = 0;
                        unsigned all = 1;
                        for (int k = 0; k < taps; k++) {
                            const int ik = fetchIndex4(s, (u16 + offU[k]) >> 16,
                                                          (v16 + offV[k]) >> 16);
                            all &= (s.clutNZ >> ik) & 1;
                            const unsigned short c = clutPix[ik];
                            ar += c & 31; ag += (c >> 5) & 31; ab += (c >> 10) & 31;
                        }
```

A PSX 15bpp pixel is `r | g<<5 | b<<10 | stp<<15`. Bit 15 — the per-texel semi-transparency
flag — is not a colour channel and is not accumulated by that loop. Rebuilding the pixel
from `ar`, `ag`, `ab` alone therefore yields bit 15 clear, unconditionally:

```c
                            pix = (unsigned short)((ar >> sh) | ((ag >> sh) << 5)
                                                              | ((ab >> sh) << 10));
```

*(reconstructed — this form never landed in a commit)*

Now look at what consumes it, twelve lines further down (`PsyX_SoftRas.cpp:585-588`):

```c
                        if (SEMI && (pix & 0x8000))
                            row[x] = blendPixel(row[x], pix, s.abr);
                        else
                            row[x] = pix & 0x7FFF;
```

Blending is gated on the *texel's* STP bit, not just on the primitive's semi-transparency
flag — which is correct PSX behaviour and is what allows a single texture to mix opaque and
translucent texels. Clearing bit 15 therefore silently converts "blend this texel" into
"write this texel opaque".

The car shadow is drawn by `Game/C/shadow.c` as a subdivided mesh of semi-transparent
textured quads. `PlaceShadowForCar` (`shadow.c:459`) sets up:

```c
	plotContext.colour = 80 | (80 << 8) | (80 << 16) | 0x2e000000;
	...
	plotContext.tpage = ((u_int)(*plotContext.ptexture_pages)[pft4->texture_set] | 0x40);
```

`0x2e` is `POLY_FT4` (`0x2c`) with the semi-transparency bit set, and `| 0x40` sets the
tpage's ABR field to 2 — the PSX's *back minus front* subtractive blend, the standard way
to darken. Under blending those texels subtract from the road and produce a shadow. Written
opaque, the same texels are a slab of near-black straight into the framebuffer, in the exact
shape of the shadow quad. Hence a black rectangle, not a black smudge.

This is a good illustration of a general hazard: any optimisation that rebuilds a pixel from
its *semantic* fields will silently drop the fields it does not know are there. The
accumulate-and-repack was written thinking about colour, and 15bpp PSX pixels are not only
colour.

### The fix

Carry the flag over from the centre sample, which is the one the filter falls back to
anyway:

```c
                        // Blend only where the whole footprint is opaque. On a
                        // cutout edge, fall back to the centre sample: averaging
                        // in the transparent entry would fringe the silhouette.
                        if (all) {
                            const int sh = (taps == 4) ? 2 : 4;
                            // keep the centre sample's STP bit: the average is
                            // rebuilt from colour channels only, and dropping it
                            // turns semi-transparent surfaces (the car's shadow)
                            // into opaque black
                            pix = (unsigned short)((ar >> sh) | ((ag >> sh) << 5)
                                                              | ((ab >> sh) << 10)
                                                  | (clutPix[idx] & 0x8000));
                            opaque = 1;
                        }
```

(`PsyX_SoftRas.cpp:569-579`.) `clutPix[idx]` is the unfiltered centre texel, already
modulated, so `& 0x8000` is exactly the flag the unfiltered path would have used. The
choice of the centre sample rather than, say, a majority vote across the footprint is
deliberate and cheap: within a single surface the flag is essentially constant, and the
`if (all)` guard already means the filter only runs where the whole footprint is opaque.

Two related invariants make this work and are worth naming:

- `modulate()` preserves bit 15 (`| (texel & 0x8000)` at `PsyX_SoftRas.cpp:211`), so
  pre-modulating the palette once per triangle does not destroy the flag either. The comment
  at `line 584` records this dependency.
- Transparency (`0x0000` = fully transparent) is tested separately through `clutNZ`, a
  16-bit mask built in `loadClut` from the *raw* palette entries
  (`PsyX_SoftRas.cpp:159-166`), so the pre-modulation cannot change which texels are
  skipped. Semi-transparency and full transparency are different things and are tracked
  separately.

### Verification

Visual, with an A/B that the `f` toggle makes exact: the same scene, the same frame,
filter on and off, with only the shadow expected to differ. Once fixed, it does not.

The cost of the filter itself was measured at the same time, with the same toggle:
**14.5 fps filter off, 10.0 fps filter on**. That comparison is internally valid — both
halves are the same scene seconds apart — but **14.5 is the baseline of that particular
A/B scene, not the port's headline unfiltered frame rate, which is 16.8 fps.** The two
were measured in different parts of the level; the filter A/B ran in a heavier one. Frame
load varies a lot on this content, roughly 1200 primitives standing still against
2300-3100 while driving, so a per-scene baseline is not a per-port number. While actually
driving, with the filter on, the measured rate is 5-9 fps.

---

## Defect 9 — red and blue exchanged all the way to the panel

*(Found by review, not by observation. The repair is in the current tree; the broken form
below is quoted from the comment that now sits above the fix.)*

### Symptom

The whole game, every frame, with red and blue swapped: an orange sky over blue asphalt.
Not subtle — and yet it survived for months, because nothing in the debugging apparatus
could see it and the content is stylised enough that a wrong-but-consistent palette reads
as "the LCD looks a bit off" rather than "these two channels are transposed".

### Why every instrument missed it

Look again at the four instruments at the top of this document. `p`, `v`, `r` and `t` all
read `vram[]` — the emulated PSX framebuffer — and reconstruct an image *on the host*, in
Python, from those 15bpp words. The conversion to the panel's format is not in that path.
Every screen dump was therefore correct, at every stage of the investigation, while the
panel was showing something else. The `HAVANA.LEV` byte-identical comparison in the
bisection section is still true and still proves what it claimed; it just proves it about
a pipeline that ends one transform earlier than the pixels a person looks at.

### The code, and the false premise underneath it

PSX VRAM packs a pixel as `STP | B<<10 | G<<5 | R` — **red in the low bits**. RGB565 puts
**red in the high bits**. The original conversion widened green and byte-swapped for the
SPI link, and left red where it was, on the stated grounds that the ST7789's MADCTL
colour-order bit would do the swap in hardware for free.

It does not, because the bit is never set. `board_pins.h:25` declares

```c
#define LCD_RGB_ORDER    LCD_RGB_ELEMENT_ORDER_RGB
```

and ESP-IDF's ST7789 driver maps that value to `madctl_val = 0`, leaving `LCD_CMD_BGR_BIT`
(`1 << 3`) clear — RGB order, no swap
(`components/esp_lcd/src/esp_lcd_panel_st7789.c:84-90`). The premise was never checked
against the driver source, and the panel obligingly displayed exactly what it was sent.

### How it was actually caught

By noticing that two functions in the same file, converting the *same* source format for
the *same* panel on the *same* board, disagree. `displaySetPalette` — the 8bpp path
inherited from the OpenLara port on this hardware — has always written red into the high
bits:

```c
// display_esplcd.c:137
uint16_t rgb565 = (r << 11) | (g << 6) | b;   // 5-6-5, G gets the extra bit
```

from a BGR555 source laid out identically to PSX VRAM. Two conversions of one format to
one panel under one MADCTL setting cannot both be right. Since OpenLara demonstrably
renders correct colour on this board with this `board_pins.h`, the 555 path was the one
that was wrong. No hardware, no camera and no new instrument were needed — only the
observation that the tree contained its own counter-example.

### The fix

One helper, shared by both 555 flush paths:

```c
// display_esplcd.c:187-195
static inline uint16_t psx555toPanel(uint16_t c)
{
    const uint16_t r5 = c & 0x1F;
    const uint16_t g5 = (c >> 5) & 0x1F;
    const uint16_t b5 = (c >> 10) & 0x1F;
    // green widens 5 -> 6 by replicating its top bit
    const uint16_t v = (uint16_t)((r5 << 11) | (((g5 << 1) | (g5 >> 4)) << 5) | b5);
    return (uint16_t)((v >> 8) | (v << 8));     // the panel wants MSB first
}
```

Red is moved to bits 11-15, blue drops to bits 0-4, and green still widens 5→6 by
replicating its top bit so that 0x1F maps to 0x3F rather than 0x3E. The final byte swap
compensates for little-endian memory against MSB-first SPI. `displayFlush555` calls it at
`display_esplcd.c:211` and `displayFlush555Scaled` at `display_esplcd.c:310`, so there is
now exactly one implementation of the conversion instead of two disagreeing ones, and the
comment block at `display_esplcd.c:175-186` records what the old version did and why it
was wrong.

The cost is two extra shifts per pixel, which is unmeasurable: the present runs on core 1
and is transfer-bound — 15.4 ms of DMA against roughly 3 ms of conversion for a whole
frame — so the "free" MADCTL trick was buying nothing even if it had worked.

### Verification

By construction and by cross-check: the 555 path now produces the same channel layout as
`displaySetPalette`, which is the path that has been rendering correct colour on this
board in another project. The lasting fix is not the four lines, though — it is that the
serial dump can no longer tell you the frame is right when the panel disagrees, because
there is only one conversion left to be wrong.

---

## What this exercise says in general

Four of these nine defects — 1, 2, 3 and 8 — are the same defect wearing different clothes:
**a value left a representation that could not hold it, and the code that consumed it could
not tell.** A 16.16 attribute exceeding `int`; a gradient exceeding 16.16; a colour channel
going below zero into an OR; a 15bpp pixel rebuilt without its 16th bit.

Two of them — 5 and 6 — are the port hazard specific to reimplementing a hardware API:
**a field that means one thing on the way in and another on the way out.** `DRAWENV.tw` is
pixels when the game writes it and a 5-bit mask when the packet parser writes it; the
TILE/SPRT opcode packs orthogonal fields that a whole-opcode mask destroys.

One — 4 — is a sanity check calibrated against the wrong quantity. One — 7 — is an
undocumented NULL overload.

And one — 9 — is in a category of its own: **an assumption about hardware behaviour that
was never checked against the hardware's driver, sitting in the one stage of the pipeline
no instrument observed.** It is the only defect here that was found by reading rather than
by measuring, and the only one whose real cost was not the bug but the false confidence:
every screen dump taken during defects 1 through 8 was honestly, verifiably correct about
a frame the panel was never shown.

The instruments mattered more than the fixes. The two that paid for themselves repeatedly:

- **A dump that shares the production code path** (`SoftRas_DebugDecodeTPage`), paired with
  a dump of the raw bytes underneath it, so "the data" and "the interpretation of the data"
  can be tested independently. That pairing is what produced the pixel-identical
  `HAVANA.LEV` comparison, and that comparison is what made it rational to spend the next
  several hours on arithmetic rather than on I/O.
- **An offline replica of the arithmetic with an exact reference and per-cause
  attribution.** "The textures look wrong" is not actionable. "217 triangles have >1 texel
  error and all 217 are attributable to the `uRow` cast, with zero unexplained" is a fix.

And the counterweight, from defect 9: **know where your instruments stop.** Both of the
above tap `vram[]`. Everything downstream of `vram[]` — one function, four lines — was
outside the reach of the entire apparatus, and that is precisely where a defect visible
from across the room hid for months.

And one piece of substance worth carrying elsewhere: **unsigned integer wraparound is not a
bug to be avoided here — it is the property that makes the computation correct.** Because
unsigned arithmetic in C is exact modulo 2^32, an intermediate that overflows and a later
accumulation that brings it back into range compose to the exact answer. A floating-point
path through the same expression cannot make that promise: its out-of-range conversion is
undefined, clamps rather than wraps, and therefore poisons everything downstream. Given a
choice between a float pipeline that is "approximately right and occasionally catastrophic"
and an integer pipeline that is "exact modulo 2^32 and provably correct where it matters",
take the integer one.

---

## Reference index

| Subject | Location |
| --- | --- |
| Row-start extrapolation (`ROWSTART`) | `PsyX_SoftRas.cpp:357-370` |
| Gradient clamp (`gradToFixed`) | `PsyX_SoftRas.cpp:289-295` |
| `modulate()` two-sided clamp | `PsyX_SoftRas.cpp:200-212` |
| Triangle area limit | `PsyX_SoftRas.cpp:310-314` |
| 32-bit edge function + bound | `PsyX_SoftRas.cpp:275-281` |
| Texture window + folded 8-bit wrap | `PsyX_SoftRas.cpp:131-141`, `232-233` |
| TILE / SPRT field decode | `PsyX_SoftRas.cpp:931-969` |
| Minification filter + STP carry | `PsyX_SoftRas.cpp:446-492`, `555-580` |
| `SoftRas_DebugDecodeTPage` | `PsyX_SoftRas.cpp:752-772` |
| OT walk and length check | `PsyX_GPU.cpp:865-892` (tag sanity at `874`) |
| Reference TILE/SPRT decode (`& 0xFD`) | `PsyX_GPU.cpp:1328-1341` |
| DR_TWIN raw-field parse | `PsyX_GPU.cpp:1505-1513` |
| `DR_MOVE` / `MoveImage` rendezvous | `PsyX_GPU.cpp:1610-1640` |
| `SetDrawMove` packet build | `PsyCross/src/psx/LIBGPU.C:373-385` |
| `SetDrawEnv` texture-window encode | `PsyCross/src/psx/LIBGPU.C:352` |
| `MoveImage` NULL convention | `PsyCross/src/psx/LIBGPU.C:131` |
| GTE 11-bit screen saturation | `PsyCross/src/gte/PsyX_GTE.cpp:216-241` |
| Reference `GR_CopyVRAM` | `PsyCross/src/render/PsyX_render.cpp:1652-1665` |
| ESP32 `GR_CopyVRAM` | `esp32_port/main/esp_stubs.cpp:83-113` |
| Serial VRAM / tpage dumps | `esp32_port/main/esp_platform.cpp:88-132` |
| PSX 555 → panel conversion | `esp32_port/main/display_esplcd.c:175-195`, used at `211`, `310` |
| 8bpp palette conversion (the cross-check) | `esp32_port/main/display_esplcd.c:129-140` |
| Car shadow primitive setup | `src_rebuild/Game/C/shadow.c:459-494` |

Relevant commits, newest first:

```
0333687  softras: raise the minification filter threshold to 2.5 texels/pixel
feaaeb2  softras: filter minified textures                       (defect 8)
3750328  esp32: rasterise on both cores
4f049c6  softras: correctness fixes and a 3.3x faster inner loop  (defects 1-4)
6e7f3a4  ESP32 target support: no-renderer mode and PSRAM-friendly buffers
bfbed95  Software rasterizer for the PSX primitive stream         (defects 5, 6 pre-landed)
```

(REDRIVER2: `fa95b9a6` carries defect 7; `208c6e7e` adds the `f` filter toggle. Defect 9's
repair — `psx555toPanel` in `esp32_port/main/display_esplcd.c` — is in the current tree.)
