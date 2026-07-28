# Where the port cuts into the codebase

This document explains the architecture of the ESP32-S3 port: which layer of
REDRIVER2/PsyCross was replaced, exactly where the cut was made, what that
choice buys, and what it costs. It is the "why the code looks like this"
document; the rasteriser internals and the performance work are covered
elsewhere.

Everything below refers to real files. Paths are relative to the repository
root `E:/Hardware/Driver2/REDRIVER2` unless stated otherwise; PsyCross lives at
`src_rebuild/PsyCross` as a submodule.

---

## 1. The three layers as they exist upstream

REDRIVER2 is a decompilation of the PlayStation 1 game. It was written to keep
the original PSX code shape, so the game does not call a renderer — it builds
PSX display lists and hands them to the PSX SDK, exactly as the retail game
did. PsyCross ("PsyX") is a reimplementation of that SDK on top of OpenGL.

That gives three layers with two clean interfaces between them:

| Layer | Where | What it is |
|---|---|---|
| Game | `src_rebuild/Game/` | Driver 2 itself. Builds ordering tables of `P_TAG` primitives (`addPrim`, `SetDrawMove`, `SetPolyGT4`, …), calls `DrawOTag`, `LoadImage`, `StoreImage`, `gte_*`. |
| PSX SDK (PsyX) | `src_rebuild/PsyCross/src/psx/` | `LIBGPU.C`, `LIBGTE.C`, `LIBETC.C`, `LIBPAD.C`, `LIBSPU.C`, … The Psy-Q API surface, reimplemented. |
| GPU backend | `src_rebuild/PsyCross/src/gpu/`, `src/render/` | Turns the primitive stream into OpenGL draw calls, and keeps a 1 MB emulated `vram[]` array in sync with the GL side. |

The important structural fact is that **`LIBGPU.C` does almost no work**. It is
a thin dispatch layer. `DrawOTag` is eleven lines
(`src_rebuild/PsyCross/src/psx/LIBGPU.C:419`):

```c
void DrawOTag(u_long* p)
{
	do
	{
		if (g_GPUDisabledState) { ClearSplits(); return; }
		if (PsyX_BeginScene())  { ClearSplits(); }

		ParsePrimitivesLinkedList(p, 0);

		DrawAllSplits();
	} while (g_dbg_emulatorPaused);
}
```

and `DrawPrim` (`LIBGPU.C:443`) is the same thing with `singlePrimitive = 1`.
The VRAM entry points are one-liners onto the backend:

```c
int ClearImage(RECT16* rect, u_char r, u_char g, u_char b)   // LIBGPU.C:26
{ GR_ClearVRAM(rect->x, rect->y, rect->w, rect->h, r, g, b); ... }

int LoadImage(RECT16* rect, u_long* p)                        // LIBGPU.C:64
{ GR_CopyVRAM((unsigned short*)p, 0, 0, rect->w, rect->h, rect->x, rect->y); ... }

int MoveImage(RECT16* rect, int x, int y)                     // LIBGPU.C:131
{ GR_CopyVRAM(NULL, rect->x, rect->y, rect->w, rect->h, x, y); ... }

int StoreImage(RECT16* rect, u_long* p)                       // LIBGPU.C:168
{ GR_ReadVRAM((unsigned short*)p, rect->x, rect->y, rect->w, rect->h); ... }
```

So the entire graphics contract between the game and the outside world reduces
to two things: *a linked list of primitive packets*, and *a 1024x512 array of
16-bit VRAM words*. Everything else is bookkeeping.

---

## 2. The seam: `ParsePrimitive`

The port intercepts the primitive stream inside the GPU parser, at the top of
`ParsePrimitive` in `src_rebuild/PsyCross/src/gpu/PsyX_GPU.cpp:1585`:

```cpp
int ParsePrimitive(P_TAG* polyTag)
{
	const int primType = polyTag->code & 0xF0;

	int primLength = 0;

	// [softras] polygon/line/tile/sprite prims rasterize into vram[] directly;
	// env packets and VRAM ops fall through to the standard handlers below
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
		if (primLength >= 0)
			return primLength;
		primLength = 0;
	}

	switch (primType) { /* the original GL handlers, unchanged */ }
```

`SoftRas_Primitive` returns the primitive length in words if it handled the
packet, or `-1` if it did not. The contract is stated in
`src_rebuild/PsyCross/src/gpu/PsyX_SoftRas.h:15-19` (the whole header is 24
lines):

```
// Handles polygon/line/tile/sprite primitives (0x20-0x7F) by rasterizing into
// vram[]. ... Returns the primitive length in words (same contract as the GL
// handlers), or -1 for primitive types it does not own (env packets, VRAM ops,
// custom prims) — caller falls through.
```

and enforced at the top of the function
(`src_rebuild/PsyCross/src/gpu/PsyX_SoftRas.cpp:781`):

```cpp
    if (primType < 0x20 || primType > 0x70)
        return -1;  // env packets / vram ops / custom prims: standard handlers
```

The walk that feeds it is `SR_WalkOT` (`PsyX_GPU.cpp:865`), reached from
`ParsePrimitivesLinkedList` (`PsyX_GPU.cpp:826`), which is what `DrawOTag`
calls. Nothing above the parser changed.

### Why here and not somewhere else

Three properties made this the right cut.

**1. It is the narrowest point that the whole draw stream passes through.**
Every polygon, line, tile and sprite Driver 2 ever draws arrives at
`ParsePrimitive` as a packet with a type code, already in screen space (the GTE
did the transform; `LIBGTE.C` and `src/gte/PsyX_GTE.cpp` are pure integer code
and were kept as-is). Replacing the backend one layer lower — at `GR_*` — would
have meant reimplementing a triangle API that had already lost the PSX
semantics (texture pages, CLUTs, the 5-bit blend rates, the per-texel STP bit)
into a set of GL uniforms and shader branches. Replacing it one layer higher —
in the game — would have meant touching Driver 2's own code, which defeats the
point of a decompilation.

**2. It preserves `vram[]` as the single source of truth.** The rasteriser
writes finished 15-bit pixels into the same `unsigned short vram[1024*512]`
array that `LoadImage`, `StoreImage`, `MoveImage` and `ClearImage` already
operate on. Because the seam is *above* that array rather than *below* it,
every path in the game that reads pixels back keeps working with no special
handling. On the GL backend those readbacks are a genuine problem — pixels live
in a framebuffer object and have to be pulled back with
`GR_StoreFrameBuffer` / `GR_ReadFramebufferDataToVRAM` before the game can look
at them. Here there is nothing to synchronise.

**3. The fall-through costs nothing and keeps the parser honest.** Draw
environment packets (`0xE0`), `DR_LOAD` (`0xA0`), `DR_MOVE` (`0x00` subtype 1)
and the Psy-X custom primitives (`0xB0`) keep using the upstream handlers. In
particular `ProcessDrawEnv` (`PsyX_GPU.cpp:1486`) is the single place that
decodes `DR_TPAGE`, `DR_TWIN`, `DR_AREA` and `DR_OFFSET` into `activeDrawEnv`,
and the software rasteriser reads that same state back out in `loadEnvState`
(`PsyX_SoftRas.cpp:102`). There is exactly one implementation of the GPU state
machine, shared by both backends. That is why the PC build can run the software
rasteriser and the GL backend against the same frame and diff them, which is how
it was developed (`PSYX_SOFTRAS=1` on PC; commit `bfbed95` in PsyCross).

### What reads VRAM back, concretely

This is the part that would have broken under a lower cut, so it is worth being
specific about who actually does it in this build:

| Call site | What it reads | Why |
|---|---|---|
| `src_rebuild/Game/C/sky.c:523` | `StoreImage` of a 16x10 rect | Sun brightness. Counts how many pixels in the sun's on-screen footprint are white (`0xFFFF` / `0x7FFF`) to decide the lens-flare intensity. |
| `src_rebuild/Game/C/sky.c:669` | `SetDrawMove(sample_sun, &source, 1008, 456)` | The rect above is first blitted VRAM→VRAM into an unused corner of VRAM (1008, 456) **as an ordering-table primitive**, so it is sampled at the right point in the frame. |
| `src_rebuild/Game/C/objanim.c:260` | `StoreImage` of a 16x1 CLUT | Palette cycling: the CLUT is read out of VRAM, rotated in place, and uploaded again with `DR_LOAD`. |
| `src_rebuild/Game/C/pres.c:508` | `StoreImage2` (`StoreClut2`) | Reads a 16-entry CLUT to set its transparency flags. |

An honest note, because the "rear-view mirror" framing is a natural one to
reach for: **Driver 2 has no rear-view mirror readback.** The mirror-shaped
case in this codebase — save a screen rect, draw over it, restore it — is the
frontend's button highlight (`src_rebuild/Game/Frontend/FEmain.c:1493` and
`:1533`, the `In` / `Out` `DR_MOVE` pair), and it is compiled out under
`#ifdef PSX`. The general point still holds and is still load-bearing: the sun
sampling above is a real VRAM→VRAM blit followed by a real VRAM readback, in
the middle of a frame, and it works unchanged because the seam sits above
`vram[]`.

It is load-bearing enough that it forced a design constraint on the dual-core
rasteriser. `DR_MOVE` reads pixels *both* cores are writing and ignores the
clip rect entirely, so it cannot simply run twice; it goes through a rendezvous
(`PsyX_GPU.cpp:1628`):

```cpp
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

`DR_LOAD` gets the same treatment (`PsyX_GPU.cpp:1673`) because `objanim.c`
uploads cycled CLUTs that the other core may be fetching from. Roughly thirteen
of these occur per frame, so the barrier cost is negligible.

---

## 3. The layering, as a diagram

```
  src_rebuild/Game/                     Driver 2 itself. UNMODIFIED except for
  ┌───────────────────────────────┐     four small #ifdef ESP32_PORT guards.
  │ draw.c  car.c  sky.c  pres.c  │
  │ objanim.c  Frontend/FEmain.c  │
  └───────────────┬───────────────┘
                  │  addPrim(ot, poly) / SetDrawMove / SetPolyGT4 ...
                  │  → an ordering table of P_TAG packets
                  v
  src_rebuild/PsyCross/src/psx/         Psy-Q API reimplementation. UNMODIFIED.
  ┌───────────────────────────────┐
  │ LIBGPU.C  DrawOTag  DrawPrim  │     thin dispatch; no work happens here
  │           LoadImage StoreImage│
  │           MoveImage ClearImage│
  │ LIBGTE.C  LIBETC.C  LIBPAD.C  │
  └───────────────┬───────────────┘
                  │  ParsePrimitivesLinkedList(p, singlePrimitive)
                  v
  src_rebuild/PsyCross/src/gpu/PsyX_GPU.cpp
  ┌────────────────────────────────────────────────────────────────────┐
  │  ParsePrimitivesLinkedList : 826   ── ESP32: fork band to core 1 ──┐│
  │  SR_WalkOT                 : 865   walk OT_TAG linked list         ││
  │  ParsePrimitive            :1585   ◄══════ THE SEAM ══════════     ││
  └───────┬──────────────────────────────────────────────┬─────────────┘│
          │ code & 0xF0 in 0x20..0x70                    │ everything    │
          │ (POLY / LINE / TILE / SPRT)                  │ else          │
          v                                              v               │
  PsyX_SoftRas.cpp                          ProcessDrawEnv (0xE0)        │
  ┌──────────────────────────┐              DR_LOAD (0xA0)  ─┐           │
  │ SoftRas_Primitive  : 774 │              DR_MOVE (0x00/1) ├─ barrier  │
  │  rasterTri<FMT,G,S>: 299 │              ProcessPsyXPrims (0xB0)      │
  │  drawSprite/drawTile/line│                     │                     │
  └────────────┬─────────────┘                     │                     │
               │                     ┌─────────────┴───────────┐         │
               │                     │ DEAD ON THIS TARGET:    │         │
               │                     │ AddSplit         : 693  │         │
               │                     │ MakeVertex*/Texcoord*   │         │
               │                     │ ProcessFlatPoly  :1133  │         │
               │                     │ ProcessGouraudPoly:1232 │         │
               │                     │ ProcessTileAndSprt      │         │
               │                     │ DrawAllSplits    : 777  │         │
               │                     │   → g_vertexBuffer      │         │
               │                     │   → GR_DrawTriangles    │         │
               │                     └─────────────────────────┘         │
               │                                                         │
               v                                                         │
  ┌────────────────────────────────────────────────────────────┐         │
  │   unsigned short vram[1024 * 512]   — 1 MB, PSRAM .bss      │◄────────┘
  │   esp32_port/main/esp_host.cpp:254                          │  join
  │   (upstream: PsyCross/src/render/PsyX_render.cpp:478)       │
  └───────────────┬─────────────────────────────┬───────────────┘
                  │                             ▲
                  │ present                     │ GR_ClearVRAM / GR_CopyVRAM /
                  v                             │ GR_ReadVRAM  (esp_stubs.cpp,
  esp32_port/main/esp_platform.cpp              │  implemented for real)
  ┌───────────────────────────────┐             │
  │ SoftRas_PresentESP()          │   LIBGPU.C ─┘  ClearImage/LoadImage/
  │ BGR555 → RGB565, rescale      │                StoreImage/MoveImage
  │ → displayPresentAsync (core 1)│
  └───────────────┬───────────────┘
                  v
        ST7789 320x240, SPI @ 80 MHz
```

On the PC build the same picture holds, except the bottom-right box is
`src/render/PsyX_render.cpp` + `glad.c` driving OpenGL, and the bottom-left is
`SoftRas_Present()` blitting `vram[]` through a self-contained GL 3.3 shader
(`PsyX_SoftRas.cpp:1038`).

---

## 4. What became dead code, and why it was left in place

Once `SoftRas_Primitive` claims types `0x20`–`0x70`, the entire GL vertex path
in `PsyX_GPU.cpp` is unreachable on this target. That is roughly 900 lines:

- `MakeVertexTriangle` / `MakeVertexQuad` / `MakeVertexRect` / `MakeLineArray`
  (`:154`–`:318`) — build `GrVertex` records in screen space.
- `MakeTexcoordTriangle` / `MakeTexcoordQuad` / `MakeTexcoordRect` and their
  `*Zero` variants (`:320`–`:563`) — turn tpage/CLUT/UV into normalised
  texture coordinates and LUT indices for the fragment shader.
- `MakeColour*` (`:565`–`:669`), `TriangulateQuad` (`:671`).
- `AddSplit` (`:693`) — the batching heuristic. It compares the current blend
  mode, texture format, texture id, clip rect and `dfe` flag against the open
  split and starts a new one on any change, because each of those is a GL state
  change.
- `ProcessFlatPoly` (`:1133`), `ProcessGouraudPoly` (`:1232`),
  `ProcessFlatLines` (`:918`), `ProcessGouraudLines` (`:1079`),
  `ProcessTileAndSprt` (`:1328`) — the per-type packet decoders that feed the above.
- `DrawSplit` (`:748`) and `DrawAllSplits` (`:777`) — issue the `glDrawArrays`.

Two explicit guards stop the dead bookkeeping from doing damage rather than
just doing nothing.

`DrawAllSplits` returns immediately (`PsyX_GPU.cpp:777`):

```cpp
void DrawAllSplits()
{
	// [softras] nothing was accumulated into the vertex buffer — the split
	// bookkeeping in ParsePrimitivesLinkedList would otherwise hand the GL
	// path a bogus vertex range
	if (g_softRasterEnabled)
		return;
```

and the per-tag `numVerts` update inside the OT walk is skipped
(`PsyX_GPU.cpp:894`):

```cpp
			// dead under the software rasterizer (nothing emits GL vertices),
			// and it is a shared write both cores would race on
			if (!g_softRasterEnabled)
			{
				GPUDrawSplit& lastSplit = g_splits[g_splitIndex];
				lastSplit.numVerts = g_vertexIndex - lastSplit.startVertex;
			}
```

The second guard is not merely tidiness: `g_vertexIndex` and `g_splitIndex` are
file-scope integers, and on the dual-core path both cores are inside
`SR_WalkOT` at once. Leaving the write in would be a genuine data race on a
value nobody reads.

### Why keep it compiled at all

Three reasons, in order of weight:

1. **`PsyX_GPU.cpp` is shared with upstream.** It is one file serving the GL
   backend, the GLES backend, the Emscripten build and this one. Deleting the
   vertex path would fork it permanently and make merging upstream changes into
   `SoftwareEscarlata/PsyCross` painful.
2. **The PC build needs both.** The software rasteriser was developed on the PC
   precisely so its output could be diffed against the GL backend frame by
   frame. That only works if both live in the same binary and are selected at
   runtime by `g_softRasterEnabled`.
3. **It costs flash, not RAM or cycles.** The dead functions are never called,
   so they cost nothing at runtime. What they *did* cost was `.bss`, and that
   was fixed directly rather than by deletion (`PsyX_GPU.cpp:59`):

```cpp
#ifdef ESP32_PORT
#define MAX_DRAW_SPLITS	 4      // GL split batching is unused here
#else
#define MAX_DRAW_SPLITS	 4096
#endif

#ifdef ESP32_PORT
// The GL path is unused on this target (the software rasterizer writes vram[]
// directly), but these buffers are still referenced by the split bookkeeping.
// 1.9MB of them cannot live in internal SRAM.
#include "esp_attr.h"
EXT_RAM_BSS_ATTR GrVertex g_vertexBuffer[MAX_VERTEX_BUFFER_SIZE];
EXT_RAM_BSS_ATTR GPUDrawSplit g_splits[MAX_DRAW_SPLITS];
```

together with `MAX_VERTEX_BUFFER_SIZE` dropping from 65536 to 64
(`PsyCross/include/PsyX/PsyX_render.h:122`, inside the `ESP32_PORT` guard that
opens at `:119`). With `USE_PGXP=0` a `GrVertex` is
20 bytes, so the vertex buffer alone was 1.25 MB; the splits array carried the
rest of the ~1.9 MB quoted in the commit, because a `GPUDrawSplit` embeds an
entire `DRAWENV` (which itself embeds a 15-word `DR_ENV`). After the change the
two arrays together are a few kilobytes, and they are in PSRAM `.bss` rather
than internal SRAM. **Nothing reads them.** They exist only so the code
referencing them still links.

The one piece of the "dead" path that is not quite dead is `ProcessPsyXPrims`
(`PsyX_GPU.cpp:1556`), which handles the Psy-X texture-override packet
`DR_PSYX_TEX`. Driver 2 emits it for the hi-res font and digit atlases
(`Game/C/pres.c:106`, `:130`, `Game/Frontend/FEmain.c:134`). It self-disables:
those call sites are all guarded by an early return on a null texture id
(`gHiresFontTexture` in `pres.c`, `gHiresFEFontTexture` in `FEmain.c`), and
the texture ids come from `GR_CreateRGBATexture`, whose ESP32 implementation
(`esp32_port/main/esp_stubs.cpp:71`) returns 0. So the packets are never
emitted and the override never fires. This is a real limitation — the
high-resolution font is simply absent on this target — not a piece of cleverness.

---

## 5. What had to be implemented for real

Everything the game reaches through `LIBGPU.C` that is *not* a drawing
primitive still has to work. `esp32_port/main/esp_stubs.cpp` splits the `GR_*`
surface in two.

**No-ops** — the GL device management, which has no analogue here
(`esp_stubs.cpp:17`–`:33` and `:60`–`:71`):

```cpp
int  GR_InitialisePSX() { return 1; }
int  GR_InitialiseRender(char*, int, int, int) { return 1; }
void GR_BeginScene() {}
void GR_EndScene() {}
void GR_SwapWindow() {}
void GR_UpdateVRAM() {}
void GR_StoreFrameBuffer(int, int, int, int) {}
void GR_ReadFramebufferDataToVRAM() {}
...
void GR_SetBlendMode(BlendMode) {}
void GR_SetupClipMode(const RECT16*, int) {}
void GR_SetTexture(TextureID, TexFormat) {}
void GR_UpdateVertexBuffer(const GrVertex*, int) {}
void GR_DrawTriangles(int, int) {}
TextureID GR_CreateRGBATexture(int, int, u_char*) { return 0; }
```

`GR_StoreFrameBuffer` and `GR_ReadFramebufferDataToVRAM` being no-ops is the
clearest statement of what the seam bought: on the GL backend those exist
solely to copy pixels *back* from the GPU into `vram[]` so that `StoreImage`
works. Here the pixels never left.

**Implemented for real** — the VRAM operations, which are pure memory work
(`esp_stubs.cpp:73`):

```cpp
// VRAM ops are pure memory work — implement them for real
void GR_ClearVRAM(int x, int y, int w, int h, unsigned char r, unsigned char g, unsigned char b)
void GR_CopyVRAM(unsigned short* src, int x, int y, int w, int h, int dst_x, int dst_y)
void GR_ReadVRAM(unsigned short* dst, int x, int y, int dst_w, int dst_h)
```

`GR_CopyVRAM` carries the most subtlety, and it is where a real bug lived. Its
signature is overloaded by the SDK: `LoadImage` passes a linear source image,
`MoveImage` passes `NULL` to mean "the source is VRAM itself, at (x, y)". The
first version dereferenced `src` unconditionally, which is exactly the sun
sampling path from §2 and produced a black screen. The fix
(`esp_stubs.cpp:83`):

```cpp
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
    ...
```

Note the wraparound masks (`& (VRAM_WIDTH - 1)`, `& (VRAM_HEIGHT - 1)`) in the
inner loops: PSX VRAM addressing wraps, and the sun blit deliberately targets
(1008, 456), close enough to the edges that a 16-wide rect can run off them.

`vram[]` itself moved. Upstream it is a file-scope array in
`PsyCross/src/render/PsyX_render.cpp:478`, a file this build does not compile
(it is `glad.c`-dependent). It is redefined at `esp32_port/main/esp_host.cpp:254`:

```cpp
// emulated PSX VRAM (normally in PsyX_render.cpp, which is host-only here).
// 1MB in PSRAM: textures are read through the data cache.
EXT_RAM_BSS_ATTR unsigned short vram[VRAM_WIDTH * VRAM_HEIGHT];
```

1 MB will not fit in the ~20 KB of internal SRAM left after the application, so
it lives in octal PSRAM and is reached through the 64 KB data cache. This is
the single most important memory decision in the port, and it was tested rather
than assumed: redirecting the framebuffer writes to internal SRAM changed the
inner loop from 96 to 95 cycles per pixel. The rasteriser is ALU-bound, not
memory-bound, and a band-rendering redesign was rejected on that evidence.
Texture *fetches* are a different story — they cost about 34 cycles per pixel,
measured by stubbing the fetch out — and that is cache-miss dominated, which is
why the 16-entry CLUT is pulled into `SRState` once per primitive
(`PsyX_SoftRas.cpp:153`).

`esp_host.cpp` covers the rest of the PsyX host surface that normally comes
from the SDL layer: `PsyX_BeginScene` / `PsyX_EndScene` (`:68`, `:82`), the
vblank clock derived from `esp_timer` instead of an interrupt thread (`:156`),
the pad reports (`:123` — Driver 2 reads the raw `PADRAW` buffers registered by
`PadInitDirect`, not `PadGetState`), logging to the serial console, and
deliberate no-ops for CD, SPU, XA and FMV hardware that is not present.

---

## 6. Compile-time configuration

All of it lives in `esp32_port/main/CMakeLists.txt`:

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
| `PSYX_NO_RENDERER=1` | Selects the "no GL backend" branch in `PsyCross/include/PsyX/PsyX_render.h:175`. |
| `USE_PGXP=0` | `VERTTYPE` stays `short`; the PGXP vertex cache and its float shadow of the GTE disappear. |
| `USE_EXTENDED_PRIM_POINTERS=1` | Primitive `addr` fields are real `uintptr_t` pointers, not 24-bit PSX addresses. |
| `ESP32_PORT` | Target guard: PSRAM placement, per-core draw env, barriers, buffer sizing. |
| `SOFTRAS_PROFILE=1` | Compiles the per-core cycle counters into `ParsePrimitive` and the present path. |
| `NTSC_VERSION`, `RELEASE` | Upstream game configuration, unchanged. |

### `PSYX_NO_RENDERER`

Before this port, `PsyX_render.h` had exactly two options and `#error`-ed
otherwise. The change (PsyCross commit `6e7f3a4`) adds a third
(`PsyX_render.h:172`):

```c
#if defined(RENDERER_OGLES) || defined(RENDERER_OGL)
typedef uint TextureID;
typedef uint ShaderID;
#elif defined(PSYX_NO_RENDERER)
// Software-rasterizer-only build (embedded targets): no GPU objects exist,
// but the GR_* API surface is kept so the rest of PsyX still compiles.
typedef uint TextureID;
typedef uint ShaderID;
#else
#error "no renderer selected: define RENDERER_OGL, RENDERER_OGLES or PSYX_NO_RENDERER"
#endif
```

The point is narrow but real: the `GR_*` **declarations** stay, so all the code
that calls them still compiles; only the definitions move to
`esp_stubs.cpp`. `TextureID` remains an integer handle that always happens to
be zero. Nothing else in PsyX had to learn that there is no GPU.

### Why `USE_PGXP` must be off

PGXP is PsyCross's precision-geometry extension: it keeps a floating-point
shadow of every GTE-transformed vertex so the GL backend can render
perspective-correct, sub-pixel-accurate geometry instead of the PSX's affine,
integer-snapped output. It is a *quality* feature that only makes sense when a
GPU is doing the interpolation. Four separate things make it unusable here.

**1. It changes the vertex type in every primitive struct.** With `USE_PGXP=1`,
`PsyCross/include/PsyX/common/pgxp_defs.h:14-22`:

```c
// in C++, VERTTYPE can be declared as half
// use `_HF` to convert to half whenever you in C mode.
#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
typedef half VERTTYPE;
#define _HF(x) x
#else
typedef short VERTTYPE;
#define _HF(x) to_half_float(x)
#endif
```

`half` is a software half-float — a struct with a `float` conversion operator
(`include/PsyX/common/half_float.h`). Every `p->x0` in `SoftRas_Primitive`
would become a half→float→int round trip, and the rasteriser's edge functions
are deliberately integer (PSX coordinates are 11-bit signed, so every term
stays under 2^25, which is what lets the half-space test run in `int` instead
of `long long`). Half-floats would poison the seam's input at its narrowest
point.

**2. It costs 1.75 MB of RAM.** `src/gte/PsyX_GTE.cpp:287`:

```cpp
PGXPVData g_pgxpCache[1 << sizeof(ushort)*8];
```

That is 65536 entries of 28 bytes (`uint lookup; float px, py, pz, scr_h, ofx,
ofy;`) = 1.75 MB, randomly indexed per vertex. The board has 8 MB of PSRAM
total, already carrying a 2 MB PSX RAM arena, 1 MB of VRAM and the game's own
buffers — and random access into a 1.75 MB table is a data-cache disaster.

**3. It adds work to every single primitive.** With PGXP on, `addPrim` itself
grows a call (`include/psx/libgpu.h:207`):

```c
#if USE_PGXP && USE_EXTENDED_PRIM_POINTERS
#define setpgxpindex(p, i)	(((P_TAG *)(p))->pgxp_index = (u_short)(i))
#define addPrim(ot, p)		setaddr(p, getaddr(ot)), setaddr(ot, p), setpgxpindex(p, PGXP_GetIndex(1))
#else
#define addPrim(ot, p)		setaddr(p, getaddr(ot)), setaddr(ot, p)
#endif
```

and `LIBGTE.C` maintains the float shadow on every transform. Game logic plus
the software GTE already accounts for 27 ms of a 59 ms frame; this is the last
place to add floating-point work.

**4. The rasteriser would ignore it anyway.** The software rasteriser is
deliberately affine, because that is what the PSX did — see the model comment at
`PsyX_SoftRas.cpp:3`. Perspective-correct vertices would produce a *different*
picture, not a better one, and would cost more to compute.

`USE_PGXP=0` with `USE_EXTENDED_PRIM_POINTERS=1` is not an exotic combination:
it is what the upstream Win32 CI already exercises, which is why the whole
codebase compiles cleanly under it (REDRIVER2 commit `f7cba554`).

### Why `USE_EXTENDED_PRIM_POINTERS` must be on

This one is not optional in the other direction. Setting it to 0 restores the
authentic PSX packet layout (`include/psx/libgpu.h:356`, with `P_LEN` at
`:363`):

```c
#define DECLARE_P_ADDR_PTAG \
	unsigned addr : 24; \
	unsigned len : 8;
#define P_LEN		1		// 1 long
```

A 24-bit `addr` field is a PSX physical address. On any host — x86, ARM or
Xtensa — the ordering table holds real pointers into a 32-bit address space, so
the extended form is required (`libgpu.h:343`):

```c
#define DECLARE_P_ADDR \
		uintptr_t addr; \
		uint len : 16; \
		uint pgxp_index : 16;
#define P_LEN		2		// 2 longs
```

`P_LEN = 2` is then baked into the arithmetic in `SR_WalkOT`
(`PsyX_GPU.cpp:880`, `:885`) and into every `return` value in
`SoftRas_Primitive`, which report primitive lengths in words *excluding* the
tag. The `pgxp_index` half-word is still present in the layout with
`USE_PGXP=0` — it is simply never written or read. The `static_assert`s
throughout `libgpu.h` (for example `sizeof(SPRT) / 4 - P_LEN == 4` at `:572`)
check that the packet sizes the game hardcodes still match after all this
substitution, at compile time, on every build.

### Source selection

The same `CMakeLists.txt` decides which upstream translation units exist at
all. The exclusions are as much a part of the architecture as the seam:

```cmake
# LIBCD emulates a CD drive with an SDL spooler thread; game data is
# memory-mapped flash here, so the CD API is stubbed instead (esp_stubs.cpp)
list(FILTER SRC_PSX EXCLUDE REGEX "LIBCD")
...
# fs.cpp needs POSIX glob() for directory enumeration — the flash VFS has no
# directories, so it is replaced by stubs in esp_stubs.cpp
list(FILTER SRC_UTILS EXCLUDE REGEX "video_source|audio_source|VideoPlayer|fs\\.cpp")
list(FILTER SRC_GAME_C EXCLUDE REGEX "xaplay")
```

`src/gpu/*.cpp` and `src/gte/*.cpp` are compiled in full; `src/render/` (the GL
backend and `glad.c`) and `src/PsyX_main.cpp` (the SDL main loop) are not
globbed at all. `Game/ASM/*.c` — the portable C versions of the PSX assembly
routines, including the renderer inner loops and RNC decompression — is
required and is included.

One upstream convention survives intact: the game's `.c` files are compiled as
C++, as the PC build does, via `set_source_files_properties(... LANGUAGE CXX)`,
guarded by `if(NOT CMAKE_BUILD_EARLY_EXPANSION)` because that command is
illegal during ESP-IDF's requirement-scan pass.

---

## 7. The force-included header

The last piece of the compile-time configuration is not a define but a
`-include`:

```cmake
set_source_files_properties(${GAME_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-w;-fpermissive;-Wno-narrowing;-include${CMAKE_CURRENT_LIST_DIR}/esp_compat.h")
```

`esp32_port/main/esp_compat.h` is prepended to **every** game and PsyX
translation unit. It is deliberately tiny, and every line in it is there
because the alternative was editing game source:

```c
#define fopen(name, mode) ((FILE*)esp_game_fopen((name), (mode)))
```

The game opens data files with plain `fopen` in dozens of places. Rather than
patch them, `fopen` is redirected to the read-only VFS over memory-mapped flash
(`esp32_port/main/esp_fs.c`), which does DOS-separator and case-insensitive
matching, and puts writes in PSRAM RAM-files.

```c
const void* esp_fs_map(const char* name, unsigned int* size);
```

Declared here so the one place that needs zero-copy access can use it. The
level spooler originally did `malloc(16 * 1024 * 1024)` for city data
(`Game/C/spool.c:1434`); on this board that allocation is impossible, and since
every reader only `memcpy`s out of the pointer, it now uses the flash mapping
in place for zero RAM.

```c
#define _stricmp  strcasecmp
#define _strnicmp strncasecmp
#define HOME_ENV "HOME"
#define _mkdir(p) (0)
```

MSVC/POSIX helpers newlib lacks, plus the save-path shim: the VFS has no
directories and there is no `HOME`, so `config.dat` and `progress.dat` land in
the RAM-file area.

```c
#include "esp_attr.h"
#define D2_PSRAM EXT_RAM_BSS_ATTR
```

This is the one that reaches back into game code. `D2_PSRAM` marks a large,
cold array for PSRAM `.bss` instead of internal SRAM. It is defined in the
force-included header precisely so it is visible in every game translation unit
without adding an include to each of them. The 2 MB PSX RAM arena uses the same
mechanism directly — `g_allocatedMem` is declared at `Game/C/system.c:122`,
inside the guard that opens at `:120`:

```c
#ifdef ESP32_PORT
#include "esp_attr.h"
EXT_RAM_BSS_ATTR char g_allocatedMem[0x200000];   // 2MB PSX RAM arena -> PSRAM
#else
char g_allocatedMem[0x200000];
#endif
```

The total footprint of `#ifdef ESP32_PORT` inside `src_rebuild/Game/` — counted
with a grep over that tree — is **four** sites, in three files: the PSRAM arena
above (`system.c:120`), one `free()` that must not run on an mmap'd pointer
(`system.c:863`), the spooler mapping (`spool.c:1434`), and `trap()` no longer
emitting x86 `int3` (`driver2.h:64`). A fifth guard often counted alongside
them, the `MAX_VERTEX_BUFFER_SIZE` sizing, is not in the game tree at all — it
is in PsyCross's `include/PsyX/PsyX_render.h:119`. The same grep over
`src_rebuild/PsyCross/` finds eleven sites across four files:
`src/gpu/PsyX_GPU.cpp` (5), `src/gpu/PsyX_SoftRas.cpp` (3),
`src/gpu/PsyX_GPU.h` (2) and `include/PsyX/PsyX_render.h` (1). That asymmetry
is the point of the seam: the target-specific code lives in the backend, not in
Driver 2. Everything else the port needed was done at the seam, in the backend,
or in the build system.

A small companion set of shims sits beside it: `esp32_port/main/SDL.h` (message
boxes to the serial log, `SDL_Delay` onto `vTaskDelay`, and types for debug
paths that are never called) plus one-line `SDL_scancode.h` / `SDL_timer.h` /
`SDL_messagebox.h`.

---

## 8. What the seam made possible downstream

Two later pieces of work are direct consequences of cutting here rather than
lower, and are worth naming because they explain some of the ESP32-specific
code visible in `PsyX_GPU.cpp`.

**Dual-core rasterisation.** Because `ParsePrimitive` is a pure function of the
packet bytes plus a small amount of GPU state, the *same* ordering table can be
walked twice concurrently, each walk clipped to a horizontal band of VRAM.
`ParsePrimitivesLinkedList` forks (`PsyX_GPU.cpp:848`):

```cpp
#ifdef ESP32_PORT
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
#endif
```

Three things had to become per-core for that to be sound, and all three are
consequences of state that lives at or just above the seam: the primitive
render state `SRState` is now `__thread` (`PsyX_SoftRas.cpp:87`); the draw
environment became `DRAWENV g_drawEnv[2]` (`PsyX_GPU.h:29`) selected by core id
in the `activeDrawEnv` macro (`:35`), because `DR_TPAGE` / `DR_TWIN` / `DR_AREA` mutate it
as the walk proceeds and the two cores are at different points in the list; and
the barriers described in §2. The result was 13.5 → 16.8 fps, verified
byte-identical to the single-core render — 0 of 19200 sampled pixels differ —
with the per-core `DRAWENV` replay checked with a `memcmp` canary at every join
(`esp32_port/main/esp_raster.cpp:114`) that has never fired.

**A texture filter the real PSX never had.** Because the seam owns
interpolation, adding a minification filter was a change to one pixel kernel
rather than a shader rewrite. It matters on this hardware: the PSX takes one
texel per pixel, a CRT low-passed the resulting aliasing away, and a 2.8" LCD
does not. Measured on the actual primitive stream, a frame contains two very
different populations — ground/buildings/foliage at 1.2–1.6 texels per pixel on
100–200 pixel polygons, and the player car at 11–23 texels per pixel on 2–3
pixel polygons — so the kernel scales with the compression ratio. It is toggled
live with `f` on the serial console, and that toggle is how its cost was
measured: 14.5 fps with the filter off against 10.0 fps with it on, seconds
apart in the same scene. Note what the 14.5 is and is not. It is the unfiltered
baseline **of that particular A/B scene**, which was a heavier part of the
level; it is not the 16.8 fps headline. Frame load varies a lot — roughly 1200
primitives standing still against 2300–3100 while driving — so the two numbers
are not comparable and only the pair is. While actually driving with the filter
on, the measured rate is 5–9 fps.

For reference, where the frame time goes at 16.8 fps (59 ms/frame): rasterisation
32 ms on the slower of the two core bands, game logic plus the software GTE
27 ms, panel present 0 ms on the critical path because it was moved to a task on
core 1. The present is transfer-bound — 320x240x2 bytes at 80 MHz SPI is 15.4 ms
and it measured 17 ms — so moving it off the game task was almost pure win.

---

## 9. Honest assessment of the seam

**What it got right.** The cut is above `vram[]`, so the readback paths work
untouched; it is below the game, so Driver 2's own code is essentially
unmodified (four `#ifdef`s in `src_rebuild/Game/`, none of them graphics); and
it is at a point where
one shared implementation of the PSX GPU state machine serves both backends, so
the PC build remains a reference the device output can be diffed against.

**What it costs.**

- Dead weight in the binary. ~900 lines of GL vertex code compile into the
  firmware and never run. This is flash, not RAM or cycles, and it is the price
  of not forking `PsyX_GPU.cpp`.
- Two `if (g_softRasterEnabled)` branches on paths that are hot-ish
  (`ParsePrimitive` runs per primitive). They are perfectly predicted and the
  measured cost is inside the noise, but they are a runtime check for something
  that is a compile-time fact on this target.
- The GL split bookkeeping still partly executes — `AddSplit` is never reached,
  but `ClearSplits` is still called from `LIBGPU.C` on every `DrawOTag`. It
  writes three integers, so this is a note for completeness rather than a
  problem.
- The `DR_PSYX_TEX` override path is inert, so the hi-res font and digits are
  unavailable. That is a genuine missing feature, not a design decision.
- The barriers in §2 serialise two primitive types across both cores. At about
  thirteen per frame it does not show up in the profile, but it is a hard
  synchronisation point in an otherwise lock-free design, and any future
  primitive with a side effect outside its band must be added to that list by
  hand. Nothing in the type system enforces it.

**What is unresolved.** The `g_softRasterEnabled` flag is a runtime global set
once in `esp32_port/main/esp_main.cpp:84`:

```cpp
    g_softRasterEnabled = 1;   // no OpenGL here — the rasterizer owns the frame
```

Its default comes from `getenv("PSYX_SOFTRAS")` (`PsyX_SoftRas.cpp:27`), which
is meaningless on an ESP32 and returns 0 — hence the explicit assignment before
the game task starts. On a target where there is no alternative backend, this
would be better as a compile-time constant so the dead branches fold away
entirely. It has not been changed because the same source file has to keep
serving the PC build, where the flag genuinely is a runtime choice.
