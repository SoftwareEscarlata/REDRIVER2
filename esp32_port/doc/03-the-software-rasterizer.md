# The software rasterizer

Everything in this document lives in one file:

```
src_rebuild/PsyCross/src/gpu/PsyX_SoftRas.cpp   (1117 lines)
src_rebuild/PsyCross/src/gpu/PsyX_SoftRas.h     (24 lines, the contract)
```

Paths are relative to the REDRIVER2 checkout root (`E:/Hardware/Driver2/REDRIVER2`).
Line numbers refer to the state of the file at commit `0333687` of the PsyCross
fork (branch `esp32s3`).

This is the component the ESP32-S3 port exists around. REDRIVER2 is a
decompilation of Driver 2 that renders through PsyCross ("PsyX"), a
reimplementation of the PSX SDK whose GPU backend translates the PlayStation
primitive stream into OpenGL draw calls. There is no OpenGL on an ESP32-S3, and
there is no GPU. So the backend was replaced: `SoftRas_Primitive()` takes the
same primitive packets and rasterizes them straight into the emulated VRAM
array, in software, on a 240 MHz Xtensa LX7.

The result runs the game at 16.8 fps on the panel. Getting there took the inner
loop from 221 cycles per covered pixel to 68 — which ends at 89 ms per frame and
11.1 fps — then moving the panel present onto core 1 (74 ms, 13.5 fps), and only
then rasterising on both cores (59 ms, 16.8 fps). This document explains the
code that does the work, in the order the data flows through it, and gives the
measurements that justify each decision. Section 9.2 has the full progression;
it is worth reading before attributing any of those steps to the wrong change.

---

## 1. The contract

The header is short and worth reading in full, because everything else follows
from it (`PsyX_SoftRas.h:15-20`):

```c
// Handles polygon/line/tile/sprite primitives (0x20-0x7F) by rasterizing into
// vram[]. `tag` is a P_TAG* (void* here: P_TAG is a typedef of an anonymous
// struct and cannot be forward-declared). Returns the primitive length in
// words (same contract as the GL handlers), or -1 for primitive types it does
// not own (env packets, VRAM ops, custom prims) — caller falls through.
int SoftRas_Primitive(void* tag);
```

Three things are being promised:

1. **It writes only to `vram[]`.** The emulated VRAM is `unsigned short
   vram[1024*512]`, `extern`-declared at `PsyX_SoftRas.cpp:24`. Upstream it is
   defined in `PsyX_render.cpp:478`; this build does not compile that file, so
   the definition moves to `esp32_port/main/esp_host.cpp:254`. Pixels are
   BGR555: `r | g<<5 | b<<10 | stp<<15`. 1024 × 512 × 16 bits is exactly 1 MB,
   which is not going to fit in 512 KB of internal SRAM, so on the ESP32 the
   array lives in octal PSRAM and is reached through the data cache; the panel
   present reads the DISPENV rectangle back out of it. (The 2 MB figure that
   turns up elsewhere in the port is `g_allocatedMem`, the PSX *main*-RAM
   arena — a different array entirely.)

2. **It is a drop-in for the GL handlers.** `ParsePrimitive()` in
   `PsyX_GPU.cpp:1593-1606` calls it first and falls through to the original
   code on `-1`:

   ```c
   if (g_softRasterEnabled)
   {
       primLength = SoftRas_Primitive(polyTag);
       if (primLength >= 0)
           return primLength;
       primLength = 0;
   }
   ```

   So environment packets (`0xE0`), `DR_LOAD` (`0xA0`), `DR_MOVE`, and PsyX's
   own custom primitives (`0xB0`) still go through the code that was already
   there and already correct. The software rasterizer only owns the primitives
   that produce pixels.

3. **The return value is a cursor step, not a status.** Section 8 covers this;
   it is the one part of the interface where being wrong by one word corrupts
   the rest of the frame rather than producing a visibly wrong triangle.

The model it implements is the PSX GPU's, and the file's own header comment
(`PsyX_SoftRas.cpp:3-8`) states it as four rules:

| Rule | Consequence for the implementation |
|---|---|
| screen coords = vertex + `DRAWENV.ofs`, in VRAM coordinate space | there is no separate "screen"; the draw offset is added at vertex decode (`mkVert`, line 742) |
| scissor = `DRAWENV.clip`, also VRAM space | the clip rectangle is a VRAM window, and on the ESP32 it is further intersected with the current core's band |
| interpolation is affine | no perspective divide, no 1/w — the PSX had none, and reproducing its texture warping is *correct*, not a shortcut |
| ordering is painter's, far-to-near, via the reverse ordering table | no depth buffer, no depth test, no sorting inside the rasterizer — order comes entirely from the caller's walk |

The last one is load-bearing for the dual-core work: because ordering comes from
the walk and every pixel belongs to exactly one horizontal band, two cores can
walk the same ordering table simultaneously and still produce the painter
ordering the game depends on.

On the PC build the rasterizer is selected with `PSYX_SOFTRAS=1`
(`readSoftRasEnv`, line 27) so it can be diffed against the GL backend on the
same data. On the ESP32 there is no choice — `esp_main.cpp:84` sets
`g_softRasterEnabled = 1` with the comment "no OpenGL here — the rasterizer owns
the frame".

---

## 2. Per-primitive state: `SRState`

Every primitive begins with `loadEnvState()` (line 788), which snapshots the
current draw environment into a single structure (lines 61-81):

```c
struct SRState
{
    int clipX0, clipY0, clipX1, clipY1;  // inclusive, VRAM space
    int ofsX, ofsY;
    int texBaseX, texBaseY;
    int texFmt;
    int abr;
    int clutX, clutY;
    // texture window (nocash: t = (t & ~(mask*8)) | ((off & mask)*8))
    int twMaskU, twMaskV, twOffU, twOffV;
    unsigned short clut16[16];
    unsigned int clutNZ;             // bit i set when clut16[i] != 0 (opaque)
    const unsigned short* texPage;   // &vram[texBaseY * VRAM_WIDTH + texBaseX]
    int slot;                        // which core's band this primitive is in
};
```

This is not a cache. It is rebuilt from scratch for every primitive — 2300-3100
times per frame while driving, against about 1200 standing still. That sounds
wasteful and is not: the
cost is dominated by `loadClut()`, sixteen halfword reads, and the alternative —
reading the same fields out of `activeDrawEnv` inside the pixel loop — is
catastrophically worse for reasons covered in section 6.2.

### 2.1 Why it is thread-local

```c
#ifdef ESP32_PORT
static __thread SRState st;
#else
static SRState st;
#endif
```
`PsyX_SoftRas.cpp:86-90`

The ESP32 port runs the rasterizer on both cores. `ParsePrimitivesLinkedList`
(`PsyX_GPU.cpp:848-859`) forks the walk:

```c
if (g_softRasterEnabled && SoftRas_ForkEnabled())
{
    SoftRas_ForkOT(p);      // hand the same list to the worker on core 1
    SR_WalkOT(p);           // this core takes the top band
    SoftRas_JoinOT();
    return;
}
```

Both cores traverse the *identical* ordering table, but they are at different
points in it at any instant. A single file-scope `SRState` would therefore let
one core rasterize a triangle with the other core's texture page, CLUT, and
clip rectangle — a data race whose symptom is not a crash but a wrong texture on
a random polygon, once in a while. Making the state per-thread removes the
sharing outright.

The same reasoning was applied one level up, to the draw environment itself. It
became an array indexed by core id (`PsyX_GPU.h:29-38`):

```c
extern DRAWENV g_drawEnv[2];
#ifdef ESP32_PORT
#define activeDrawEnv (g_drawEnv[xPortGetCoreID()])
#else
#define activeDrawEnv (g_drawEnv[0])
#endif
```

Both cores replay the same `DR_TPAGE` / `DR_TWIN` / `DR_AREA` packet stream into
their own slot and must therefore end bit-identical. That is asserted at every
join (`esp_raster.cpp:114-117`) with a `memcmp` and reported in the profile line
as `diverge`; it has been observed to be zero.

`__thread` under ESP-IDF resolves to per-task TLS reached through the thread
pointer register, so every access carries an extra indirection compared with a
plain global. The rasterizer pays it exactly once per primitive: `rasterTri`
copies the whole structure into a local at line 430 and the pixel loop touches
only the local. `xPortGetCoreID()` is one `rsr.prid`, also read once per
primitive (`PsyX_GPU.h:33-34` documents this).

### 2.2 `loadEnvState`

```c
static void loadEnvState()
{
    const int slot = xPortGetCoreID();
    st.slot = slot;

    st.clipX0 = activeDrawEnv.clip.x;
    st.clipY0 = activeDrawEnv.clip.y;
    st.clipX1 = activeDrawEnv.clip.x + activeDrawEnv.clip.w - 1;
    st.clipY1 = activeDrawEnv.clip.y + activeDrawEnv.clip.h - 1;
    ... clamp to VRAM ...
    if (st.clipY0 < g_srBandY0[slot]) st.clipY0 = g_srBandY0[slot];
    if (st.clipY1 > g_srBandY1[slot]) st.clipY1 = g_srBandY1[slot];
    st.ofsX = activeDrawEnv.ofs[0];
    st.ofsY = activeDrawEnv.ofs[1];
    ...
}
```
`PsyX_SoftRas.cpp:102-142`

The clip rectangle is converted from `(x, y, w, h)` to inclusive bounds, clamped
into VRAM, and then intersected with the current core's horizontal band. The
band defaults span all of VRAM (lines 98-99), so with the fork disabled this is
bit-identical to single-core rendering — the band mechanism costs two compares
and disappears.

One detail in the comment at lines 120-122 is worth repeating because it is the
kind of thing that produces a bug you cannot reproduce:

> The two bands partition the WHOLE of VRAM rather than the clip rect, so a
> `DR_AREA` packet arriving mid-walk can never move pixels outside their union.

`DR_AREA` packets are embedded in the ordering table and change the clip
rectangle as the walk proceeds. If the bands had been derived by splitting the
clip rectangle in half, a mid-walk `DR_AREA` would move the clip rectangle out
from under a partition computed for the old one, and pixels could land in the
gap or in the overlap. Partitioning all 512 VRAM lines makes the split
independent of anything the packet stream can change: `esp_raster.cpp:93-94`
gives core 0 lines `0 .. splitY-1` and core 1 lines `splitY .. 511`,
unconditionally.

`slot` is also where per-band accounting comes from. At the end of each triangle
(line 620) the rasterizer publishes its span pixel count:

```c
g_srBandPx[s.slot] += spanPx;   // drives the band balancer at the join
```

The split line is not fixed at half the screen — the top of a Driver 2 frame is
sky and the bottom is road, so the pixel load is nowhere near even. After the
join, `esp_raster.cpp:121-128` moves the split toward whichever core drew fewer
span pixels, capped at 32 lines per frame and skipped on near-empty frames so it
cannot oscillate. It settles at 29 ms versus 32 ms.

### 2.3 The texture window

The last four lines of `loadEnvState` are the texture window, and they are the
densest four lines in the file:

```c
st.twMaskU = ~(activeDrawEnv.tw.w * 8) & 0xFF;
st.twOffU  = (activeDrawEnv.tw.x & activeDrawEnv.tw.w) * 8;
st.twMaskV = ~(activeDrawEnv.tw.h * 8) & 0xFF;
st.twOffV  = (activeDrawEnv.tw.y & activeDrawEnv.tw.h) * 8;
```
`PsyX_SoftRas.cpp:138-141`

**Why `tw` holds raw packet fields.** `DRAWENV.tw` is a `RECT16`, so its members
are named `x, y, w, h` and every other consumer in PsyX would read them as a
pixel-space rectangle. They are not. The `DR_TWIN` decoder never converts back
to pixel space (`PsyX_GPU.cpp:1505-1512`):

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

Those are the four 5-bit fields of the GP0(`E2h`) word exactly as the hardware
defines them: `w` is *Mask X*, `h` is *Mask Y*, `x` is *Offset X*, `y` is
*Offset Y*, all in units of 8 texels. The encoder on the other side confirms it
— `LIBGPU.C:352` builds the packet from a pixel-space window as
`((256 - tw.h) >> 3)` and `(tw.x >> 3)`.

So `tw` in a live `DRAWENV` carries GPU-encoded mask/offset, and the only
correct interpretation is the hardware formula:

```
texcoord = (texcoord AND NOT(Mask*8)) OR ((Offset AND Mask)*8)
```

**Why it needs no "window disabled" special case.** With `Mask == 0` the formula
reduces to `t & 0xFF | 0`, which is the identity within 8 bits. There is no
branch to predict and no flag to test.

**Why the 8-bit wrap folds into it for free.** PSX texture coordinates are 8-bit
and wrap inside the 256×256 page. `~(Mask*8)` is a full-width complement, so the
`& 0xFF` that truncates it to a byte *is* the wrap. The masked and OR-ed bits
are provably disjoint: `(Offset & Mask) ⊆ Mask` bitwise, and shifting both left
by 3 preserves that, so `(Offset & Mask)*8 ⊆ Mask*8`, which is exactly the
complement of `twMask`. The OR can therefore never carry past 255. That removed
two AND instructions from every single texel fetch.

**Worked example.** A 64×64 window at texture-page offset (128, 64):

| quantity | value |
|---|---|
| Mask X = `(256-64)>>3` | 24 = `0b11000` |
| Offset X = `128>>3` | 16 = `0b10000` |
| `twMaskU = ~(24*8) & 0xFF` = `~192 & 0xFF` | `0x3F` |
| `twOffU = (16 & 24) * 8` = `16*8` | 128 |

A coordinate `u = 200` becomes `(200 & 0x3F) | 128 = 8 | 128 = 136`. Checked
against the intent: 200 wrapped into a 64-wide window is `200 mod 64 = 8`, plus
the window origin 128, giving 136. Correct, in two instructions.

### 2.4 `loadTpage` and `loadClut`

```c
static inline void loadTpage(int tpage)
{
    st.texBaseX = SR_TPAGE_TX(tpage);
    st.texBaseY = SR_TPAGE_TY(tpage);
    st.texFmt   = SR_TPAGE_FMT(tpage);
    st.abr      = SR_TPAGE_ABR(tpage);
    st.texPage  = &vram[st.texBaseY * VRAM_WIDTH + st.texBaseX];
}
```
`PsyX_SoftRas.cpp:144-151`

`texPage` is precomputed here rather than in the fetch. On Xtensa, materialising
the address of `vram` requires an `l32r` literal-pool load; keeping the page base
in a register across a whole triangle removes that from the per-pixel path.

```c
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
`PsyX_SoftRas.cpp:153-167`

Pulling the 16-entry palette out of VRAM once per primitive is a cache decision,
and the comment at lines 71-73 states it precisely:

> VRAM lives in slow external RAM and the framebuffer writes evict it from the
> data cache, so reading the CLUT per pixel costs a miss; 16 entries fit in
> registers/stack.

This is the specific shape of the ESP32-S3's memory system biting. The data
cache is 64 KB with 64-byte lines, and the rasterizer streams framebuffer writes
through it continuously. A 32-byte palette that is read once per pixel and never
written is exactly the kind of data that a streaming write pattern evicts
between uses. Hoisting it out of VRAM into 16 stack slots (which are in internal
SRAM) converts a probable PSRAM round trip into a load that hits.

Note that only 4bpp palettes are cached. An 8bpp CLUT is 256 entries; caching it
per primitive would cost more than it saves, so the 8bpp path in `fetchTexel`
still does the dependent VRAM read.

`clutNZ` is discussed in section 6.3 — it exists to keep transparency semantics
exactly right once the palette is pre-modulated.

---

## 3. PSX texture and CLUT address encodings

Four macros decode the 16-bit `tpage` word (`PsyX_SoftRas.cpp:56-59`):

```c
#define SR_TPAGE_TX(tp)   (((tp) & 0xF) * 64)
#define SR_TPAGE_TY(tp)   ((((tp) >> 4) & 1) * 256)
#define SR_TPAGE_FMT(tp)  (((tp) >> 7) & 3)     // 0=4bit 1=8bit 2=15bit
#define SR_TPAGE_ABR(tp)  (((tp) >> 5) & 3)     // blend rate
```

| bits | field | decoded to |
|---|---|---|
| 0-3 | page X | X base in **VRAM halfwords**, `0 … 960` in steps of 64 |
| 4 | page Y | Y base in VRAM lines, 0 or 256 |
| 5-6 | ABR | semi-transparency mode, see section 6.4 |
| 7-8 | format | 0 = 4bpp CLUT, 1 = 8bpp CLUT, 2 = 15bpp direct |

The units matter and are a classic source of confusion. `texBaseX` is a
**halfword** (VRAM cell) index, not a texel index. VRAM is 1024 halfwords wide,
so the 16 page slots across are 64 cells each. How many *texels* that is depends
on the format:

| format | texels per VRAM cell | page width in texels | page width in cells |
|---|---|---|---|
| 4bpp | 4 | 256 | 64 |
| 8bpp | 2 | 256 | 128 |
| 15bpp | 1 | 256 | 256 |

That table explains an asymmetry in `fetchTexel` that would otherwise look like
an inconsistency: a 4bpp page occupies exactly its 64-cell slot and can never
run off the right edge of VRAM, whereas an 8bpp page is 128 cells wide and a
15bpp page 256, so a page based at X slot 15 (cell 960) extends to cell 1087 or
1215 and *must* wrap. The real GPU wraps; so must this.

`SR_TPAGE_TY` decodes only bit 4. The Psy-Q `getTPage` macro
(`libgpu.h:230-232`) also packs a bit 11 for `y & 0x200`, which addresses a
second 512-line half of VRAM that does not exist on a 1024×512 machine. Ignoring
it is correct here.

The CLUT word is decoded at `PsyX_SoftRas.cpp:155-156`:

```c
st.clutX = (clut & 0x3F) << 4;      // X in halfwords, multiple of 16
st.clutY = (clut >> 6) & 0x1FF;     // full 9-bit VRAM line
```

which inverts `getClut(x, y) = ((y) << 6) | (((x) >> 4) & 0x3f)`
(`libgpu.h:234-235`). A palette is therefore always 16-halfword aligned
horizontally and can sit on any VRAM line.

### 3.1 Worked example, end to end

Take a 4bpp texture page whose data starts at VRAM (640, 256), blend mode 0,
and build the word with the repo's own macro:

```
getTPage(0 /*4bpp*/, 0 /*abr*/, 640, 256)
  = ((0 & 3) << 7)          = 0x000
  | ((0 & 3) << 5)          = 0x000
  | ((256 & 0x100) >> 4)    = 0x010     <- page Y bit
  | ((640 & 0x3ff) >> 6)    = 0x00A     <- page X = slot 10
  | ((256 & 0x200) << 2)    = 0x000
  = 0x1A
```

Decoding it back:

```
SR_TPAGE_TX(0x1A)  = (0x1A & 0xF) * 64      = 10 * 64 = 640   cells
SR_TPAGE_TY(0x1A)  = ((0x1A >> 4) & 1) * 256 = 256            lines
SR_TPAGE_FMT(0x1A) = (0x1A >> 7) & 3         = 0              4bpp
SR_TPAGE_ABR(0x1A) = (0x1A >> 5) & 3         = 0              (B+F)/2
```

`loadTpage` therefore sets `texPage = &vram[256*1024 + 640]`. Now fetch the
texel at `(u, v) = (37, 12)` with no texture window:

```
cell index = (v << 10) + (u >> 2) = (12 * 1024) + 9 = 12297
absolute   = 256*1024 + 640 + 12297 = 275081
           = VRAM (x = 649, y = 268)
nibble     = (u & 3) * 4 = 1 * 4 = 4      -> bits 4..7 of that halfword
```

Sanity check against the intent: texel row 12 of a page based at line 256 is
VRAM line 268 ✓; texel column 37 is cell `37/4 = 9` past the base cell 640, so
cell 649 ✓, and it is the second of the four texels in that cell, i.e. nibble 1
✓.

A matching CLUT at VRAM (832, 500):

```
getClut(832, 500) = (500 << 6) | ((832 >> 4) & 0x3F) = 32000 | 52 = 32052
clutX = (32052 & 0x3F) << 4 = 52 << 4 = 832  ✓
clutY = (32052 >> 6) & 0x1FF = 500           ✓
```

---

## 4. Texel fetch

```c
template <int FMT>
static inline unsigned short fetchTexel(const SRState& s, int u, int v)
{
    u = (u & s.twMaskU) | s.twOffU;
    v = (v & s.twMaskV) | s.twOffV;

    if (FMT == 0) // 4-bit CLUT
    {
        return s.clut16[(s.texPage[(v << 10) + (u >> 2)] >> ((u & 3) * 4)) & 0xF];
    }
    else if (FMT == 1) // 8-bit CLUT
    {
        int x = s.texBaseX + (u >> 1);
        unsigned short cell = vram[((s.texBaseY + v) & 511) * VRAM_WIDTH + (x & 1023)];
        int idx = (cell >> ((u & 1) * 8)) & 0xFF;
        return vram[s.clutY * VRAM_WIDTH + ((s.clutX + idx) & 1023)];
    }
    else // 15-bit direct
    {
        return vram[((s.texBaseY + v) & 511) * VRAM_WIDTH + ((s.texBaseX + u) & 1023)];
    }
}
```
`PsyX_SoftRas.cpp:220-253`

Four things to note.

**`FMT` is a template parameter, not an argument.** Each of the three bodies is
compiled separately and the dead branches vanish. There is no per-pixel switch
on the texture format anywhere in the triangle path.

**`s` is a reference to a caller-local copy, and this is not stylistic.** The
comment at lines 216-219 gives the reason:

> `s` is deliberately a reference to a CALLER-LOCAL copy of the render state,
> not the `st` global: the rasterizer writes through `vram`, and the compiler
> cannot prove those writes do not alias `st`, so reading the global here forces
> a reload of every field on every pixel.

This is not a compiler deficiency; it is the C++ aliasing rules working as
specified. `SRState` contains `unsigned short clut16[16]`, and the framebuffer
store is `row[x] = pix` through an `unsigned short*`. Those two types are
compatible, so a store through the framebuffer pointer *may legally* modify
`st.clut16`, `st.twMaskU`, and everything else. Any read of the global after a
store must therefore be re-issued. Copying into a local whose address never
escapes (line 430, `const SRState s = st;`) lets the compiler keep the fields in
registers for the whole triangle.

**The 4bpp path has had its bounds masks deleted, provably.** Lines 236-239:

> `texBaseY` is 0 or 256 and `v <= 255`, so the row never leaves the page;
> `texBaseX <= 960` and `u>>2 <= 63`, so the column never wraps either. Both of
> the old bounds masks were dead work.

`v` cannot exceed 255 because the texture window step already truncated it to 8
bits, and `u >> 2 <= 63` for the same reason. This is only sound because the
window masking happens first — a good example of how folding the wrap into the
mask (section 2.3) pays twice.

The 8bpp and 15bpp paths keep `& 511` and `& 1023` because, as section 3 showed,
their pages genuinely can overhang the right edge of VRAM.

**Cost.** The measured figure on the device is about **34 cycles per pixel** for
the texture fetch, obtained by stubbing the fetch out entirely
(`SOFTRAS_NOTEX`, lines 223-227, returns `0x1234 + u + v` and renders garbage on
purpose) and re-measuring. For scale, the whole inner loop after optimisation is
68 cycles per covered pixel — so texture fetch is roughly half of it. The 8bpp
path is materially worse than that number: it does two dependent VRAM reads
(texel cell, then palette entry), and the second one has no cached palette
behind it.

### 4.1 `fetchIndex4`

```c
static inline int fetchIndex4(const SRState& s, int u, int v)
{
    u = (u & s.twMaskU) | s.twOffU;
    v = (v & s.twMaskV) | s.twOffV;
    return (s.texPage[(v << 10) + (u >> 2)] >> ((u & 3) * 4)) & 0xF;
}
```
`PsyX_SoftRas.cpp:257-262`

Identical to `fetchTexel<0>` except that it stops at the palette *index* instead
of resolving it. That is what makes the pre-modulated-CLUT fast path possible
(section 6.3): with the index in hand, the final pixel is a single array lookup
into a table computed once per triangle, and the transparency test is a bit test
on `clutNZ` rather than a comparison against a fetched colour.

---

## 5. The triangle rasterizer

```c
template <int TEXFMT, bool GOURAUD, bool SEMI>
static void rasterTri(SRVert v0, SRVert v1, SRVert v2)
```
`PsyX_SoftRas.cpp:298-624`

`TEXFMT` is `-1` for untextured or `0/1/2` for the three texture formats, so the
template expands to 16 distinct pixel kernels (4 untextured × 12 textured).
`drawTri` (line 627) picks one from the runtime flags with a nested switch that
runs once per primitive. This costs instruction footprint on a chip with a
32 KB instruction cache — a trade-off that has not been measured either way, and
is noted here as an open question rather than a settled one.

Vertices arrive as:

```c
struct SRVert { int x, y; int u, v; int r, g, b; };
```

with `mkVert` (line 742) having already added the draw offset, so `x, y` are
VRAM coordinates.

### 5.1 Winding and degenerate rejection

```c
int area = (v1.x - v0.x) * (v2.y - v0.y) - (v1.y - v0.y) * (v2.x - v0.x);
if (area == 0)
    return;
if (area < 0)
{
    SRVert t = v1; v1 = v2; v2 = t;
    area = -area;
}
if (area > 64 * 1024 * 1024)
    return;
```
`PsyX_SoftRas.cpp:302-314`

The PSX draws both windings — it has no backface culling in the GPU — so the
rasterizer normalises instead of rejecting: swap two vertices and negate. The
vertices are taken **by value** precisely so this swap is local and the caller's
quad decomposition is unaffected.

The area limit has history. The commit that fixed it records:

> The area limit rejected one half of a legitimately screen-spanning quad,
> leaving a diagonal hole, since the two halves can straddle the bound.

The old limit was `1024*512*2`, i.e. two screens' worth. A quad that covers the
whole screen splits into two triangles whose areas need not be equal; one could
land under the bound and the other over it, and the result was a triangular hole
across the sky or the ground with a perfectly straight diagonal edge — which
looks like a rasterizer bug and is in fact a rejection heuristic. The current
bound is `64 * 1024 * 1024` = 2²⁶, which sits *above* anything the coordinate
range can legitimately produce: by 5.3 the cross product stays under 2²⁵. So it
no longer rejects plausible geometry at all — it is a guard against garbage
input, which is exactly what the source comment says it is ("reject only
genuinely absurd triangles").

### 5.2 Bounding box and clip

Lines 316-327 compute the vertex bounding box, intersect it with
`st.clipX0..clipX1 / clipY0..clipY1` (which already include the core's band),
and return early if empty. Everything downstream — row starts, span solving,
attribute row starts — is expressed relative to `(minX, minY)` of the *clipped*
box. That has one consequence that caused a real bug, covered in 5.5.

### 5.3 Edge functions and the top-left rule

```c
static inline int edgeFn(const SRVert& a, const SRVert& b, int px, int py)
{
    return (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
}
```
`PsyX_SoftRas.cpp:278-281`

This is the z-component of `(b - a) × (p - a)`. With the winding normalised so
that `area = edgeFn(v0, v1, v2) > 0`, a point is inside the triangle exactly
when all three edge functions are `>= 0`.

Note the type: `int`, not `long long`. The comment at lines 275-277:

> 32-bit is sufficient and much cheaper than 64-bit on Xtensa: PSX primitive
> coords are 11-bit signed and the draw offset adds at most 2047, so every term
> here stays under 2^25.

Work the bound through: an 11-bit signed coordinate is at most ±1024 and the
draw offset adds at most 2047, so a coordinate lies in roughly ±2047 and a
*difference* of two of them is bounded by about 4094. Each product is therefore
bounded by about 4094 × 4094 ≈ 1.7 × 10⁷, and the difference of two such
products stays under 2²⁵ — which is what the comment claims, and comfortably
inside `int32`. On a 32-bit
Xtensa, a `long long` multiply-subtract is a multi-instruction sequence with
register pressure; this was one of the changes that took the inner loop from 221
to 68 cycles per pixel.

The three edges are set up as linear functions of `(x, y)`:

```c
const int A01 = v0.y - v1.y, B01 = v1.x - v0.x;
const int A12 = v1.y - v2.y, B12 = v2.x - v1.x;
const int A20 = v2.y - v0.y, B20 = v0.x - v2.x;
```
`PsyX_SoftRas.cpp:330-332`

`A` is the coefficient of `x` and `B` the coefficient of `y`: stepping one pixel
right adds `A`, stepping one row down adds `B`. That is why the row loop ends
with `w0row += B12; w1row += B20; w2row += B01;` (line 615) and never
recomputes an edge function.

**The fill rule.** Lines 335-337:

```c
const int bias0 = (A12 > 0 || (A12 == 0 && B12 > 0)) ? 0 : -1;
const int bias1 = (A20 > 0 || (A20 == 0 && B20 > 0)) ? 0 : -1;
const int bias2 = (A01 > 0 || (A01 == 0 && B01 > 0)) ? 0 : -1;
```

This is the standard top-left rule, expressed in terms of the edge coefficients
rather than vertex comparisons. Read it as:

- `A12 > 0` means `v1.y > v2.y`, i.e. the edge runs *upward* on a y-down screen —
  a **left** edge for this winding. Include pixels exactly on it: bias 0.
- `A12 == 0 && B12 > 0` means the edge is horizontal and runs to the right — a
  **top** edge. Include it: bias 0.
- Anything else is a right or bottom edge. Bias `-1`, which turns the test
  `w >= 0` into `w >= 1`: pixels exactly on the edge are excluded.

The bias is added once into the row-start accumulators (lines 339-341), so the
span solver's `w >= 0` test already carries it and the rule costs nothing
per pixel.

Why it matters here specifically: every PSX quad is drawn as two triangles that
share a diagonal (`drawTri(a,b,c)` then `drawTri(b,d,c)`, lines 820-821). Without
a consistent fill rule the pixels on that diagonal are either drawn twice or not
at all. Drawn twice is not merely wasted work — for a semi-transparent quad it
double-blends and the diagonal becomes visible as a bright or dark seam. Not
drawn at all leaves a one-pixel crack. The top-left rule makes shared edges
belong to exactly one of the two triangles.

This is a modern, consistent fill rule; it is not a bit-exact reproduction of
the real GPU's edge coverage. The file is explicit about that: "exact within the
PSX 11-bit coordinate range" (line 11) refers to the arithmetic being exact, not
to hardware-identical coverage.

### 5.4 The per-scanline span solver

This is the single change with the largest effect on the inner loop. The
original code stepped three accumulators and tested three signs at every pixel
of the bounding box, including pixels outside the triangle. The replacement
solves the three half-planes once per row (`PsyX_SoftRas.cpp:494-519`):

```c
for (int y = minY; y <= maxY; y++)
{
    // Solve the three half-planes for this row rather than testing every
    // pixel: along the row, w = wRow + (x - minX) * A, and the covered
    // span is where all three stay >= 0. This lifts the coverage test out
    // of the inner loop entirely.
    int xs = minX, xe = maxX;

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
    #undef SR_CLIP_EDGE
```

**The algebra.** Fix a row `y`. Let `wRow` be the edge function evaluated at
`(minX, y)`, bias included. Because the edge function is linear in `x` with
coefficient `A`:

```
w(x) = wRow + (x - minX) * A
```

The pixel is inside this half-plane when `w(x) >= 0`. Substituting and writing
`k = x - minX >= 0`:

```
wRow + k*A >= 0
```

Three cases, which are exactly the three branches of the macro:

| case | solution | what the code does |
|---|---|---|
| `A > 0` — `w` increases along the row | `k >= -wRow / A` | if `wRow >= 0` the whole row already satisfies it, nothing to do. Otherwise `d = ceil(-wRow / A)` and the edge imposes a **lower bound** `xs = max(xs, minX + d)` |
| `A < 0` — `w` decreases along the row | `k <= wRow / (-A)` | if `wRow < 0` the row is empty even at its left end and only gets worse: `xs = xe + 1`. Otherwise `d = floor(wRow / -A)` and the edge imposes an **upper bound** `xe = min(xe, minX + d)` |
| `A == 0` — `w` is constant along the row | `wRow >= 0` or nothing | if `wRow < 0` the entire row is outside: `xs = xe + 1` |

So each edge contributes either a lower bound, an upper bound, or a whole-row
rejection, and the covered span is the intersection `[xs, xe]`. Three edges,
three bounds, one span. The inner loop is then a plain `for (int x = xs; x <= xe;
x++)` with **no coverage test at all**.

**The integer division is exact in both directions, deliberately.** C integer
division truncates toward zero, which is only `floor` for non-negative operands
and only `ceil` with the `+A-1` trick for non-negative operands. Check both
sites:

- `A > 0` branch: it is guarded by `wRow < 0`, so `-wRow > 0` and `A > 0`. Both
  operands are positive, so `(-wRow + A - 1) / A` is an exact ceiling.
- `A < 0` branch: the division is in the `else` of `wRow < 0`, so `wRow >= 0`,
  and `-A > 0`. Both operands non-negative, so truncation *is* floor.

Neither site can see a negative operand. An off-by-one here would either drop a
column of pixels along one edge of every triangle or write one column outside
it, and in a painter's-algorithm renderer the latter shows up as fringing on
polygon boundaries.

**Correctness of the empty-row shortcut.** `xs = xe + 1` uses the value of `xe`
at that moment, and a later edge can only *lower* `xe`. If it does, `xs =
xe_old + 1 > xe_new` still holds, so the row is still correctly skipped by the
`if (xs <= xe)` guard on line 521.

**Overflow.** `wRow` is bounded by ~2²⁵ (5.3) and `A >= 1`, so `d <= 2²⁵` and
`minX + d` cannot overflow `int`.

**What it buys.** Per pixel the old code paid three adds and three sign tests
plus the loop over bounding-box pixels that are not covered at all. For a thin
triangle — and the player car in Driver 2 is made of triangles covering 2-3
pixels each — the bounding box can be several times the covered area, so most of
that work was pure waste. The new code pays at most three divides per *row* and
zero per pixel. Combined with the 32-bit edge functions, the state-copy fix, the
CLUT caching and the pre-modulation, the inner loop went from **221 to 68 cycles
per covered pixel**.

Note that `spanPx` (line 523) accumulates *covered* pixels, not bounding-box
pixels, so the cycles-per-pixel figure reported by the profiler is honest about
what it divides by.

### 5.5 Affine attribute interpolation in 16.16

The PSX interpolates texture coordinates and vertex colours affinely in screen
space — no perspective correction. That is what produces the characteristic
texture warping, and reproducing it is a requirement, not an omission.

**Gradients.** Lines 353-355:

```c
#define GRAD(p0, p1, p2) \
    dDx = gradToFixed(((float)(A12 * (p0) + A20 * (p1) + A01 * (p2))) * invArea), \
    dDy = gradToFixed(((float)(B12 * (p0) + B20 * (p1) + B01 * (p2))) * invArea)
```

The derivation is short. Barycentric coordinates are `λ0 = w0/area`, `λ1 =
w1/area`, `λ2 = w2/area`, and an interpolated attribute is `λ0·p0 + λ1·p1 +
λ2·p2`. From 5.3, `∂w0/∂x = A12`, `∂w1/∂x = A20`, `∂w2/∂x = A01`. Therefore

```
∂attr/∂x = (A12·p0 + A20·p1 + A01·p2) / area
∂attr/∂y = (B12·p0 + B20·p1 + B01·p2) / area
```

which is exactly the macro. One reciprocal (`invArea`, line 344) and two
multiply-adds per attribute, computed once per triangle. There are up to five
attributes (`u, v, r, g, b`), so at most ten gradients.

**Clamping the conversion.** Lines 289-295:

```c
static inline int gradToFixed(float g)
{
    if (g >  256.0f) return  (256 << 16);
    if (g < -256.0f) return -(256 << 16);
    return (int)(g * 65536.0f);
}
```

The comment at lines 349-352 explains why this is not paranoia:

> A near-degenerate sliver makes `1/area` explode, and an out-of-range
> float→int conversion is undefined behaviour that saturates to `INT_MIN` on
> Xtensa — which would poison every pixel of the span.

`INT_MIN` as a 16.16 gradient is not a large gradient, it is a *negative* one of
enormous magnitude; the accumulator then walks off in the wrong direction and
every pixel of the primitive samples garbage. 256 texels of step per pixel is
already far past anything meaningful, so clamping loses nothing real.

**Row starts, and the bug that motivated them.** Lines 367-370:

```c
#define ROWSTART(a0, dx, dy) (int)((unsigned)((a0) << 16) \
    + (unsigned)(dx) * (unsigned)(minX - v0.x) \
    + (unsigned)(dy) * (unsigned)(minY - v0.y) \
    + (unsigned)((dx) >> 1) + (unsigned)((dy) >> 1))
```

This extrapolates the attribute from vertex 0 to the **centre** of pixel
`(minX, minY)`: the two `>> 1` terms are the half-pixel sample offset. (The code
this replaced applied a crude `+0.5`-texel bias instead, which is not the same
thing and is wrong by a fraction that depends on the gradient.)

The important part is the arithmetic type. The commit message records the
failure:

> `rasterTri` computed the row-start u/v/rgb by casting a float to int. The
> start point is the CLIPPED bounding-box corner, which for a sliver triangle
> sits far off the triangle, so the 16.16 value exceeded 2^31 and the cast —
> undefined behaviour — saturated to `INT_MIN`. That poisoned every row of the
> primitive with a constant garbage offset.

The subtlety is that `(minX, minY)` is a corner of the *clipped bounding box*,
and for a long thin triangle that corner can be nowhere near the triangle. The
extrapolated attribute value there is meaningless and can be astronomically
large — but that is fine, because it is never used directly. It is only ever a
starting point that gets `duDx` added to it repeatedly until the accumulator
reaches a pixel that is actually covered.

Doing the extrapolation in `unsigned` makes the overflow **defined**: it wraps
modulo 2³². And because the per-pixel accumulation `u16 += duDx` (line 610) wraps
the same way, the wrap cancels exactly. For any covered pixel `(x, y)` the
accumulator holds

```
(u0 << 16) + duDx·(x - v0.x) + duDy·(y - v0.y) + half   (mod 2^32)
```

and since the true value at a covered pixel is a small in-range number, the
mod-2³² representative *is* that number. `u16 >> 16` then yields the correct
texel. Signed overflow would have been undefined behaviour with no such
guarantee.

**Stepping.** Per pixel (line 610) the accumulators advance by `duDx / dvDx` and,
if gouraud, `drDx / dgDx / dbDx` — one add each, no multiplies. Per row
(lines 616-617) they advance by the `Dy` gradients. The span solver's `adv = xs -
minX` (line 524) is applied once when entering a span:

```c
const int adv = xs - minX;
int u16 = uRow + duDx * adv, v16 = vRow + dvDx * adv;
int r16 = rRow + drDx * adv, g16 = gRow + dgDx * adv, b16 = bRow + dbDx * adv;
```

so skipping the uncovered left part of the bounding box costs one multiply per
attribute per row instead of one add per attribute per skipped pixel.

---

## 6. The pixel kernels

### 6.1 Transparency

The PSX rule is that a texel whose 16-bit value is exactly `0x0000` is fully
transparent and is not written. Note what this does *not* mean: `0x8000` is
black with the STP bit set, which is opaque black (or a blended black), and is
written. The general path implements the rule directly (line 594):

```c
unsigned short texel = fetchTexel<TEXFMT>(s, u16 >> 16, v16 >> 16);
if (texel != 0)  // 0x0000 = fully transparent
{
    ...
}
```

Untextured primitives have no transparency test — every covered pixel is
written.

### 6.2 Modulation

```c
static inline unsigned short modulate(unsigned short texel, int r8, int g8, int b8)
{
    int r = ((texel & 31) * r8) >> 7;
    int g = (((texel >> 5) & 31) * g8) >> 7;
    int b = (((texel >> 10) & 31) * b8) >> 7;
    if (r > 31) r = 31; else if (r < 0) r = 0;
    if (g > 31) g = 31; else if (g < 0) g = 0;
    if (b > 31) b = 31; else if (b < 0) b = 0;
    return (unsigned short)(r | (g << 5) | (b << 10) | (texel & 0x8000));
}
```
`PsyX_SoftRas.cpp:200-212`

PSX modulation treats `0x80` (128) as identity: `(c * 128) >> 7 == c`. Values
above that brighten, below darken, and the result saturates at 31. The STP bit
is carried through unchanged, which the semi-transparency test downstream relies
on.

Both ends are clamped, and the lower clamp was a fix:

> `modulate()` clamped only the upper bound. Gouraud accumulators can run
> negative outside the triangle's colour range, and a negative channel OR-ed
> into the pixel smeared sign bits across all three channels as bright speckle.

The mechanism is worth understanding because it explains the symptom. If `r`
comes out as, say, `-3`, then `r | (g << 5) | (b << 10)` ORs `0xFFFFFFFD` into
the pixel: every channel goes to its maximum and the STP bit is set. A single
negative channel therefore produces a bright white speckle, not a dark pixel —
which is why the artefact looked like noise rather than like clipping.

The `rawTex` shortcut (line 397) detects the identity case:

```c
const bool rawTex = (TEXFMT >= 0) && cr == 0x80 && cg == 0x80 && cb == 0x80;
```

### 6.3 The pre-modulated CLUT fast path

```c
const bool FASTCLUT = (TEXFMT == 0) && !GOURAUD;
unsigned short clutPix[16];
if (FASTCLUT)
{
    for (int i = 0; i < 16; i++)
        clutPix[i] = rawTex ? s.clut16[i] : modulate(s.clut16[i], cr, cg, cb);
}
```
`PsyX_SoftRas.cpp:438-444`

For a flat-shaded 4bpp primitive — "the overwhelmingly common case in Driver 2",
line 433 — the modulation colour is constant across the triangle and the texel
takes only 16 distinct values. So the per-pixel modulation is a function of the
palette index alone, and it can be applied to the 16 palette entries once
instead of to every pixel. The comment quantifies it: "~400 cycles here against
~30 cycles on every one of the span's pixels".

`FASTCLUT` is a compile-time constant within each template instantiation, so the
kernels that are not flat 4bpp never emit any of this code.

The commit message records a sharper reason this mattered than "modulation is
expensive":

> The disassembly showed `modulate()` running unconditionally with its result
> merely selected by the `rawTex` branch, so ~30 cycles per pixel were paid and
> discarded.

That is, `pix = rawTex ? texel : modulate(texel, cr, cg, cb)` had compiled into
"compute both, select one". Even primitives that needed no modulation at all
were paying for it on every pixel. Hoisting the work to the palette removed both
the cost and the speculation.

**`clutNZ` keeps transparency bit-identical.** The problem with pre-modulating is
that `modulate()` is not injective on zero: a dark-but-nonzero palette entry
modulated by a low colour can round to `0x0000`. Testing `clutPix[idx] != 0`
would then treat an opaque texel as transparent — a subtle change in which
pixels get written, which in a painter's renderer means holes appearing in dark
geometry. So the test is done against the *raw* entry, via a bitmask built in
`loadClut`:

```c
int idx = fetchIndex4(s, u16 >> 16, v16 >> 16);
unsigned short pix = clutPix[idx];
unsigned opaque = (s.clutNZ >> idx) & 1;
```
`PsyX_SoftRas.cpp:549-553`

One shift and one AND, and, as the comment on lines 550-551 says, "transparency
semantics are identical to the old `texel != 0`".

The final write (lines 582-589):

```c
if (opaque)
{
    // modulate() preserves the STP bit, so it still reads true here
    if (SEMI && (pix & 0x8000))
        row[x] = blendPixel(row[x], pix, s.abr);
    else
        row[x] = pix & 0x7FFF;
}
```

Semi-transparency requires **both** the primitive's `abe` bit (`SEMI`, a template
parameter) and the texel's own STP bit. That is the hardware rule. The opaque
write clears bit 15 rather than propagating the mask bit; the PSX mask-bit
machinery (`MaskSetOR` / `MaskEvalAND`) is not emulated anywhere in PsyX — the
`ProcessDrawEnv` handler for it is a log line with the assignments commented out
(`PsyX_GPU.cpp:1539-1544`).

### 6.4 Semi-transparency: `blendPixel`

```c
static inline unsigned short blendPixel(unsigned short back, unsigned short front, int abr)
{
    int br = back & 31, bg = (back >> 5) & 31, bb = (back >> 10) & 31;
    int fr = front & 31, fg = (front >> 5) & 31, fb = (front >> 10) & 31;
    int r, g, b;
    switch (abr)
    {
        default:
        case 0: r = (br + fr) >> 1;  g = (bg + fg) >> 1;  b = (bb + fb) >> 1;  break;
        case 1: r = br + fr;         g = bg + fg;         b = bb + fb;         break;
        case 2: r = br - fr;         g = bg - fg;         b = bb - fb;         break;
        case 3: r = br + (fr >> 2);  g = bg + (fg >> 2);  b = bb + (fb >> 2);  break;
    }
    if (r > 31) r = 31; if (r < 0) r = 0;
    ...
}
```
`PsyX_SoftRas.cpp:173-190`

The four modes are the PSX's, selected by the `ABR` field of the texture page
word (`SR_TPAGE_ABR`, and hence `st.abr`):

| ABR | formula | typical use in Driver 2 |
|---|---|---|
| 0 | `(B + F) / 2` | glass, generic 50 % blends |
| 1 | `B + F` | additive — lights, headlight glow, sparks |
| 2 | `B - F` | subtractive — shadows, darkening |
| 3 | `B + F/4` | weak additive |

Mode 2 is why the lower clamp exists on every channel: subtraction can go
negative, and the same sign-smearing failure described in 6.2 would apply.

The divisions are integer shifts, i.e. they truncate — matching the hardware,
which does not round.

`avg2` (lines 194-197) is a SWAR average of two packed 5-5-5 pixels:

```c
return (unsigned short)((((a ^ b) & 0xFBDE) >> 1) + (a & b));
```

The mask clears the low bit of each 5-bit field so the halves cannot borrow
across field boundaries. It is currently **not called anywhere** in the file —
it is left over from an earlier form of the minification filter, which now
accumulates unpacked channels because it averages four or sixteen samples rather
than two. Worth knowing when reading the file; it is dead code, not a subtlety
you are missing. (Similarly, the `dbgTris` counter at line 35 is printed by
`dbgDump` but never incremented, and `dbgDump` itself only runs on the PC
present path.)

### 6.5 The texture minification filter

This is the one place where the rasterizer deliberately does *not* imitate the
PSX, and the reasoning is stated at lines 446-456:

> The PSX takes one texel per pixel. Distant geometry compresses a detailed
> atlas by 3-10x, so that single sample is essentially a random pick out of the
> footprint and the surface turns to noise. A CRT low-passed it away; a sharp
> LCD does not.

The frame was measured — via `SOFTRAS_DEBUG_CAR` (lines 404-425), which prints
the texel-per-pixel ratio of polygons landing where the player car sits — and it
contains two clearly separated populations:

| geometry | polygon size | texels per pixel |
|---|---|---|
| ground, buildings, foliage | 100-200 px | 1.2 - 1.6 |
| the player car | 2-3 px | 11 - 23 |

Those two populations want opposite things, which is what the kernel selection
encodes (lines 459-491):

```c
const int su = (duDx < 0 ? -duDx : duDx) + (duDy < 0 ? -duDy : duDy);
const int sv = (dvDx < 0 ? -dvDx : dvDx) + (dvDy < 0 ? -dvDy : dvDy);
const int worst = su > sv ? su : sv;

int side = 0;
if (worst > 0x28000) side = 2;
if (worst > 0x40000 && (maxX - minX + 1) * (maxY - minY + 1) <= 64) side = 4;
```

`0x28000` is 2.5 and `0x40000` is 4.0 in 16.16. So: a 2×2 kernel past 2.5 texels
per pixel; a 4×4 kernel past 4.0, **but only for triangles whose bounding box is
at most 64 pixels**. That restriction is the entire reason the 4×4 is
affordable. Extreme ratios occur on the car, whose polygons are 2-3 pixels each,
so sixteen taps on them is a rounding error in the frame budget. Road strips are
compressed just as hard but cover hundreds of pixels each, so they stay at 2×2.

The threshold started at 1.5 and was raised to 2.5 in commit `0333687`:

> Below that the single sample already lands close enough to the footprint
> average, so filtering there spent a lot of surface area for very little.
> Measured while driving, the frame carries 2300-3100 primitives rather than the
> ~1200 of a stationary shot, so the filtered surface area — and its cost — is
> much larger in motion than a static comparison suggests.

**The tap pattern follows the footprint, not a square** (lines 483-490):

```c
for (int j = 0, k = 0; j < side; j++) {
    const int dj = 2 * j + 1 - side;
    for (int i = 0; i < side; i++, k++) {
        const int di = 2 * i + 1 - side;
        offU[k] = (duDx * di + duDy * dj) >> shift;
        offV[k] = (dvDx * di + dvDy * dj) >> shift;
    }
}
```

`di` and `dj` run over `{-1, +1}` for `side == 2` and `{-3, -1, +1, +3}` for
`side == 4`, and `shift` is 2 or 3 so the offsets are quarter-steps of the
*actual* screen-axis gradients. Because the offsets are built from `duDx, duDy,
dvDx, dvDy` rather than from a fixed square, the sample pattern automatically
takes the shape of the pixel's footprint in texture space. For the road, where
vertical compression is an order of magnitude stronger than horizontal, the
pattern stretches accordingly.

**Transparency is handled by refusing to blend across a cutout edge**
(lines 555-580):

```c
int ar = 0, ag = 0, ab = 0;
unsigned all = 1;
for (int k = 0; k < taps; k++) {
    const int ik = fetchIndex4(s, (u16 + offU[k]) >> 16, (v16 + offV[k]) >> 16);
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
```

If any tap lands on a transparent palette entry the average is discarded and the
centre sample is used instead. Averaging in the transparent entry's colour would
fringe every silhouette — foliage and fences would get a halo of whatever colour
index 0 happens to hold.

The STP bit is taken from the **centre sample**, because the average is rebuilt
from colour channels only. The commit records what happens without that:

> without it the car's shadow rendered as an opaque black rectangle.

**Cost.** The filter is a per-pixel multiplier on the most expensive part of the
loop: it does `taps` extra `fetchIndex4` calls, and texture fetch measured about
34 cycles per pixel on its own. Four taps therefore roughly triples the fetch
cost on filtered surfaces. Measured end to end by flipping it live, the filter
costs **14.5 → 10.0 fps**. That pair is internally valid — both halves are the
same scene seconds apart — but the 14.5 is the unfiltered baseline **of that
particular A/B scene**, which was a heavier part of the level. It is *not* the
16.8 fps headline, and it must never be quoted as "the" unfiltered rate: frame
load varies a lot, roughly 1200 primitives standing still against 2300-3100
while driving. With the filter on, the port currently runs about 10 fps
stationary and 5-9 fps while driving. The live toggle — `'f'` on the serial
console flips `g_srFilter` (`esp_platform.cpp:311-312`) — exists precisely so
the two can be compared on the actual panel, in one scene, without a reflash.

**What it does not fix.** The commit is honest about the limit:

> This helps the ground, buildings and foliage a great deal. It helps the car
> much less: sixteen samples out of a 23×7 texel footprint is still sparse.
> Fixing that properly needs mip levels, which for 4bpp CLUT textures cannot be
> built in index space and would have to be stored per palette.

That last clause is the real obstacle: you cannot average palette *indices* —
the average of index 3 and index 9 is not a colour between them — so a mip
pyramid would have to be built in colour space and stored per (texture, palette)
pair. Driver 2 cycles palettes at runtime (`objanim.c` uploads CLUTs mid-frame),
so that is not a one-time preprocessing step.

Finally: the filter only exists in the `FASTCLUT` path. Gouraud-shaded, 8bpp and
15bpp primitives are unfiltered.

---

## 7. Sprites, tiles and lines

These are the cold paths. They exist for the HUD, menus and text, they are a
small fraction of the frame, and they have not been optimised.

**`drawSprite`** (lines 657-687) is an axis-aligned textured blit with the clip
test *inside* both loops and a `switch (st.texFmt)` *inside the inner loop*:

```c
for (int x = 0; x < w; x++)
{
    int px = x0 + x;
    if (px < st.clipX0 || px > st.clipX1) continue;
    unsigned short texel;
    switch (st.texFmt)
    {
        case 0:  texel = fetchTexel<0>(st, u0 + x, v0 + y); break;
        case 1:  texel = fetchTexel<1>(st, u0 + x, v0 + y); break;
        default: texel = fetchTexel<2>(st, u0 + x, v0 + y); break;
    }
    ...
}
```

Every one of those is avoidable — clamp the loop bounds, hoist the format
dispatch — and none of it has been done because sprites are not where the time
goes. It is also the one path that reads `st` directly rather than a local copy,
so it pays the aliasing reload discussed in section 4. Recorded here as known,
unfixed, and cheap to fix if sprite-heavy content ever appears.

Note that `drawSprite` is exactly the case where the 8-bit texture coordinate
wrap earns its keep: it samples at `u0 + x` with `x` running to `w-1`, and a wide
sprite can push `u0 + x` past 255. On hardware that wraps within the page; the
comment at lines 228-231 records what happens if it does not — "this is what
garbled wide sprites such as HUD/menu text".

**`drawTile`** (lines 689-706) is the untextured version and does clamp its
bounds up front, so its inner loop is a straight fill or a straight blend.

**`drawLine`** (lines 712-736) is a parametric DDA:

```c
for (int i = 0; i <= steps; i++)
{
    int px = x0 + dx * i / steps;
    int py = y0 + dy * i / steps;
    ...
}
```

Two integer divisions per step, plus three more per step when gouraud. The
section header calls it Bresenham; it is not — Bresenham is division-free. The
comment is stale with respect to the code. It also clips per pixel and has a
`steps > 1024` bail-out as a garbage guard. Lines are rare enough in Driver 2
that none of this has mattered.

---

## 8. Primitive dispatch, and why the return value matters

### 8.1 The walk cursor

`SoftRas_Primitive` returns the primitive's length **in 32-bit words, excluding
the packet header**. This is not informational. It is the step size for a cursor
that walks *inside* an ordering-table tag (`PsyX_GPU.cpp:869-891`):

```c
for (uintptr_t basePacket = (uintptr_t)p; ; basePacket = (uintptr_t)nextPrim(basePacket))
{
    const int tagLength = getlen(basePacket);
    if (tagLength > 0)
    {
        uintptr_t currentPacket = basePacket;
        const uintptr_t endPacket = basePacket + (tagLength + P_LEN) * sizeof(u_int);
        int primLength = 0;
        while (currentPacket < endPacket)
        {
            primLength = ParsePrimitive((P_TAG*)currentPacket);
            currentPacket += (primLength + P_LEN) * sizeof(u_int);
        }

        if (currentPacket != endPacket)
        {
            eprinterr("did not output valid primitive or ptag length is not valid (diff=%d)\n",
                      endPacket - currentPacket);
        }
    }
    if (isendprim(basePacket))
        break;
}
```

The PSX allows several primitives to be concatenated behind one ordering-table
tag (`catPrim`, `libgpu.h:216`), and the tag's `len` field covers all of them.
The outer loop follows the linked list; the **inner** loop steps through the
concatenated primitives, and the only thing telling it where the next one begins
is the value the handler returned.

So a wrong return value does not produce a wrong triangle. It leaves the cursor
pointing into the middle of a packet, and everything after it in that tag is
reinterpreted as a primitive header — arbitrary bytes decoded as a `code` byte
and a length. The rest of the tag renders as garbage or is skipped entirely. The
`currentPacket != endPacket` check catches the mismatch after the fact and logs
it, which is how such a bug would be found, but the frame is already wrong.

`P_LEN` is the header size in words: 2 on 32-bit targets, 3 on x86-64
(`libgpu.h:339-363`), because `USE_EXTENDED_PRIM_POINTERS` stores a real
`uintptr_t` link plus a length/PGXP-index word instead of the PSX's packed
24-bit address. The ESP32 is 32-bit, so `P_LEN == 2`. The handler returns only
the payload length and never has to know this.

The returned values must therefore match the `setlen` values in the Psy-Q
primitive constructors exactly (`libgpu.h:292-316`):

| code | struct | `setlen` | returned at |
|---|---|---|---|
| `0x20` | `POLY_F3` | 4 | line 831 |
| `0x24` | `POLY_FT3` | 7 | lines 805, 812 |
| `0x28` | `POLY_F4` | 5 | line 840 |
| `0x2C` | `POLY_FT4` | 9 | line 822 |
| `0x30` | `POLY_G3` | 6 | line 876 |
| `0x34` | `POLY_GT3` | 9 | line 857 |
| `0x38` | `POLY_G4` | 8 | line 885 |
| `0x3C` | `POLY_GT4` | 12 | line 867 |
| `0x40` | `LINE_F2` | 3 | line 896 |
| `0x48` | `LINE_F3` | 5 | line 903 |
| `0x4C` | `LINE_F4` | 6 | line 911 |
| `0x50` | `LINE_G2` | 4 | line 923 |
| `0x58` | `LINE_G3` | 7 | line 928 |
| `0x5C` | `LINE_G4` | 9 | **not handled** — see 8.4 |
| `0x60` | `TILE` | 3 | line 962 |
| `0x64` | `SPRT` | 4 | line 948 |
| `0x68` | `TILE_1` | 2 | line 968 |
| `0x70` | `TILE_8` | 2 | line 968 |
| `0x74` | `SPRT_8` | 3 | line 955 |
| `0x78` | `TILE_16` | 2 | line 968 |
| `0x7C` | `SPRT_16` | 3 | line 955 |

The `-1` return is the fall-through signal, and it is produced by a single range
test at the top (lines 781-782):

```c
if (primType < 0x20 || primType > 0x70)
    return -1;  // env packets / vram ops / custom prims: standard handlers
```

That covers `0x00` (`DR_MOVE`), `0xA0` (`DR_LOAD`), `0xB0` (PsyX custom),
`0xE0` (draw environment) and the VRAM transfer codes — all of which are pure
memory operations the GL backend already implemented correctly and which the
software path has no reason to duplicate. On the ESP32 two of them additionally
sit behind a rendezvous so they happen exactly once across the two cores
(`PsyX_GPU.cpp:1628-1638` and `1673-1683`).

### 8.2 Decoding the polygon codes

The polygon cases read the sub-type straight out of the code byte
(lines 794-795 for `0x20`, 845-846 for `0x30`):

```c
const bool textured = (code & 4) != 0;
const bool quad     = (code & 8) != 0;
```

with `0x20` meaning flat and `0x30` gouraud, and `semi = (code & 2)` taken once
at the top (line 779). Quads are split into two triangles in the PSX vertex
order (lines 820-821):

```c
drawTri(a, b, c, true, false, semi);
drawTri(b, d, c, true, false, semi);
```

PSX quad vertices are in a Z pattern (0,1,2,3), so the triangles are (0,1,2) and
(1,3,2) — which preserves winding and matches what the GL path does with
`MakeVertexQuad(x0, x1, x3, x2)`. The shared edge is `b-c`, handled exactly once
thanks to the fill rule (section 5.3).

### 8.3 SPRT and TILE: size and textured bits

Both `0x60` and `0x70` land in one case (lines 931-970), because on the PSX the
sprite/tile family encodes size in bits 3-4 and that field straddles the
high nibble:

```c
loadTpage(activeDrawEnv.tpage);

// bit 2 (0x04) = textured (SPRT family); bits 3-4 (0x18) = size:
// 0x00 variable, 0x08 1x1, 0x10 8x8, 0x18 16x16
const int sizeCode = code & 0x18;
const bool isSprite = (code & 0x04) != 0;
```

Checking the whole family against that decode:

| code | `& 0x04` | `& 0x18` | decoded |
|---|---|---|---|
| `0x60` TILE | 0 | `0x00` | tile, size from packet |
| `0x64` SPRT | 4 | `0x00` | sprite, size from packet |
| `0x68` TILE_1 | 0 | `0x08` | tile, 1×1 |
| `0x70` TILE_8 | 0 | `0x10` | tile, 8×8 |
| `0x74` SPRT_8 | 4 | `0x10` | sprite, 8×8 |
| `0x78` TILE_16 | 0 | `0x18` | tile, 16×16 |
| `0x7C` SPRT_16 | 4 | `0x18` | sprite, 16×16 |

The semi-transparency bit (`0x02`) and the shade-texture bit (`0x01`) occupy the
low two bits and never disturb this decode.

Two structural points. First, `loadTpage(activeDrawEnv.tpage)` — sprites and
tiles carry **no tpage field of their own**; they use whatever page the draw
environment currently holds, exactly as on hardware. Second, the three
fixed-size sprite structs share a layout, so one cast covers them (lines
950-955):

```c
// fixed-size sprites share the SPRT_8/16 layout
SPRT_8* p = (SPRT_8*)tag;
const int side = (sizeCode == 0x08) ? 1 : (sizeCode == 0x10) ? 8 : 16;
loadClut(p->clut);
drawSprite(p->x0, p->y0, side, side, p->u0, p->v0, p->r0, p->g0, p->b0, semi);
return 3;
```

This is confirmed by the static asserts in `libgpu.h:581-590`: `SPRT_8` and
`SPRT_16` are both 3 longs with identical members.

### 8.4 The SCE null-polygon tpage hack

```c
POLY_FT3* p = (POLY_FT3*)tag;
if (p->x0 == -1 && p->y0 == -1 && p->x1 == -1) // SCE null-poly tpage hack
{
    loadTpage(p->tpage);
    activeDrawEnv.tpage = p->tpage;
    return 7;
}
```
`PsyX_SoftRas.cpp:800-806`

This is not a PsyX invention. The GL path documents it at `PsyX_GPU.cpp:1170`:
"It is an official hack from SCE devs to not use DR_TPAGE and instead use null
polygon". A textured triangle with all vertices at `(-1, -1)` covers no pixels,
but the GPU still latches its `tpage` field — so it is a texture-page change
that costs one primitive slot and does not need a separate `DR_TPAGE` packet.

Driver 2 uses it. `Game/C/pause.c:1473-1484` emits exactly this, right after the
dialogue-box quad:

```c
null = (POLY_FT3*)current->primptr;
setPolyFT3(null);

null->x0 = -1;
null->y0 = -1;
null->x1 = -1;
null->y1 = -1;
null->x2 = -1;
null->y2 = -1;
null->tpage = 0;

addPrim(current->ot, null);
```

The software rasterizer's handling has two halves and both are needed.
`loadTpage(p->tpage)` updates the per-primitive state, and `activeDrawEnv.tpage =
p->tpage` writes it back into the draw environment so that later primitives which
take their page from the environment — every `SPRT`, every `TILE` (section 8.3)
— see the change. Then it returns 7, the correct `POLY_FT3` length, without
drawing.

Note the test checks three fields where the GL path's `IsNull`
(`PsyX_GPU.cpp:908-916`) checks all six. Three `-1`s in the low coordinates of a
real triangle would be a coordinate at `(-1,-1)` plus an x of `-1`; possible in
principle, and the shorter test is a deliberate cost trade in a function that
runs thousands of times per frame. It has not caused a problem.

---

## 9. How the numbers were obtained

None of the performance claims in this document came from reading the code. The
file carries four compile-time measurement switches and one debug decoder, and
each of them answered a specific question that could not be answered any other
way on a board with no profiler.

| switch | where | what it isolates |
|---|---|---|
| `SOFTRAS_NOTEX` | `fetchTexel`, lines 223-227 | returns a synthesised value instead of reading VRAM. Renders garbage on purpose. **Result: texture fetch ≈ 34 cycles/pixel.** |
| `SOFTRAS_FBSCRATCH` | lines 283-287, 527-531 | redirects all framebuffer writes to one 1024-entry row in internal SRAM. **Result: 96 vs 95 cycles/pixel — PSRAM framebuffer writes are essentially free.** |
| `SOFTRAS_PROFILE` | lines 399-401, 621-623 | counts triangles and covered span pixels; paired with cycle counters around `SoftRas_Primitive` in `PsyX_GPU.cpp:1595-1599` and reported per core |
| `SOFTRAS_DEBUG_CAR` | lines 404-425 | prints texel-per-pixel ratios for polygons landing where the player car sits. **Result: the 1.2-1.6 vs 11-23 split that shaped the filter.** |
| `SoftRas_DebugDecodeTPage` | lines 755-772 | decodes a texture page into a 128×128 image *using the rasterizer's own sampling path*, so "bad texture data" can be told apart from "bad sampling" |

The `SOFTRAS_FBSCRATCH` result deserves emphasis because it overturned a plan.
The intuition on a chip whose framebuffer lives in octal PSRAM is that the
rasterizer must be memory-bound, and the standard remedy is band rendering:
rasterize into a small internal-SRAM tile and DMA it out. That redesign was
**rejected on this measurement**. Redirecting every framebuffer write to
internal SRAM changed the cost from 96 to 95 cycles per pixel — within noise.
The inner loop is ALU-bound, and the write-combining behaviour in front of PSRAM
absorbs a sequential store stream well enough that the external memory never
becomes the constraint. Band rendering would have added complexity and bought
nothing.

The profile line is printed once per 60 frames by `esp_host.cpp:96-108`:

```c
ESP_LOGI(TAG, "  raster c0 %lu + c1 %lu ms/f  present %lu  other %lu | "
              "%lu tris/f  %lu kpx/f  %lu cyc/px  diverge %d", ...);
```

Note `other` is computed as the frame time minus `max(r0, r1)`, not minus their
sum — because the frame waits for both cores, so the critical path is the slower
band, not the total work.

### 9.1 Where the time goes at 16.8 fps

| stage | time per frame |
|---|---|
| rasterisation (slower of the two core bands) | 32 ms |
| game logic + software GTE | 27 ms |
| panel present | 0 ms (moved to core 1) |

The present is not free in absolute terms — 320×240×2 bytes over an 80 MHz SPI
link is 15.4 ms of pure transfer, and it measured 17 ms, so it was almost
entirely DMA waiting. That is why moving it to core 1 removed it from the frame
budget rather than merely reducing it: there was nothing to optimise, only
something to overlap.

### 9.2 The progression

| milestone | frame time | fps | raster |
|---|---|---|---|
| first frame that rendered on the panel | 200 ms | 5.0 | 153 ms |
| span solver + 32-bit edge functions + local render state | 109 ms | 9.2 | 65 ms |
| CLUT cached per primitive | 106 ms | 9.4 | 63 ms |
| pre-modulated CLUT, dead bounds masks, hoisted page pointer | 89 ms | 11.1 | 47 ms |
| panel present moved to core 1 | 74 ms | 13.5 | 47 ms |
| rasterisation split across both cores | 59 ms | **16.8** | 29 + 32 ms |
| with the minification filter enabled | 100-200 ms | ~10 stationary, 5-9 driving | |

Read that table carefully, because the obvious misreading is the one this
document used to make. **The rasterizer rework in this file ends at 89 ms and
11.1 fps.** Everything from the span solver down to the pre-modulated CLUT — the
whole 221 → 68 cycles-per-pixel story of sections 5 and 6 — earns that row and
no more. The step to 74 ms and 13.5 fps was not rasterizer work at all: it was
moving the panel present onto a task on core 1, which took its ~27 ms off the
game task while raster time stayed at 47 ms. Only then did splitting the
rasterisation across both cores cut that 47 ms into 29 ms on core 0 and 32 ms on
core 1; the frame waits for both, so 32 + 27 = 59 ms.

The dual-core result was validated rather than assumed: the frame is
byte-identical to the single-core render, **0 of 19200 sampled pixels differ**.
That is only possible because the rasterizer has no global mutable state left
outside `vram[]` — which is what sections 2.1 and 2.2 were about.

---

## 10. Known divergences and open items

Stated plainly, because a reader comparing this against the GL backend will hit
them.

**1. Code bit 0 (`shadeTex` / raw texture) is ignored.** The GL path reads it —
`ProcessFlatPoly` computes `shadeTexOn = (polyTag->code & 1) == 0` and forces the
vertex colour to 128,128,128 when it is set. The software path never looks at
the bit; it infers "raw" by testing whether the packet colour *happens* to be
`0x808080` (`rawTex`, line 397). For a textured primitive with the bit set and a
colour other than `0x808080` the two renderers would disagree. In Driver 2 the
only two `setShadeTex` call sites are `Game/C/pause.c:1455` and `:1534`, both on
a `POLY_F4` — an untextured primitive, where the hardware ignores the bit
anyway. The consequence is a visible but harmless difference on that one
dialogue box: the GL path fills it with mid-grey, the software path with the
packet's own colour (16,16,16). The software behaviour is the one that matches
hardware here, but the general case is unimplemented.

**2. A textured polygon does not latch its tpage into the draw environment.**
`PsyX_GPU.cpp:1168` and `:1210` set `activeDrawEnv.tpage = poly->tpage` for
*every* `POLY_FT3` and `POLY_FT4`. The software path does so only for the
null-polygon form (section 8.4); for a real textured polygon it calls
`loadTpage()`, which updates the per-primitive state but leaves the environment
alone. A `SPRT` or `TILE` following a textured polygon would therefore take its
page from an older `DR_TPAGE` rather than from that polygon. Nothing observed in
the game depends on it, and the dual-core divergence canary would not catch it
(both cores agree — they are both equally wrong), but it is a real difference
from both the GL path and the hardware.

**3. `LINE_G4` is decoded as `LINE_G3`.** The `0x50` handler (lines 916-929)
tests only `(code & 0x0C) == 0` for `LINE_G2` and treats everything else as
`LINE_G3`, returning 7. A `LINE_G4` packet (`0x5C`, length 9) would desync the
walk cursor by two words. This is currently unreachable: `setLineG4` appears
nowhere in the game source, and the GL path does not implement `LINE_G4`
rendering either (`PsyX_GPU.cpp:1124-1128` is a `TODO` that only returns the
length). Still a latent trap, and cheap to close.

**4. The mask bit is not emulated.** Opaque writes clear bit 15 (`pix & 0x7FFF`).
`MaskSetOR` / `MaskEvalAND` are not implemented anywhere in PsyX.

**5. `drawSprite` is unoptimised**: per-pixel clip tests, a per-pixel switch on
the texture format, and direct reads of the `st` global rather than a local copy
(section 7).

**6. The minification filter only covers flat 4bpp.** Gouraud, 8bpp and 15bpp
primitives are unfiltered, and the proper fix — mip levels — is blocked on 4bpp
CLUT textures not being mippable in index space, with runtime CLUT cycling
ruling out a simple per-palette bake (section 6.5).

**7. Dead code**: `avg2` (line 194) is never called; `dbgTris` (line 35) is never
incremented; the "Bresenham" comment on the line section (line 709) describes an
algorithm the code does not use.

**8. Kernel count vs instruction cache.** Sixteen template instantiations of
`rasterTri` is a deliberate trade of instruction footprint for branch-free inner
loops, on a chip with a 32 KB instruction cache. It has not been measured either
way. If the frame ever mixes many kernels rapidly, this is a place to look.
