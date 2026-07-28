# Rasterising on both cores

The ESP32-S3 has two Xtensa LX7 cores at 240 MHz. Until this change, one of them
rasterised Driver 2's entire primitive stream and the other was idle except for
short bursts of panel DMA. Putting the second core to work took the game from
13.5 to 16.8 fps.

The interesting part is not the speed-up. It is that a PSX ordering table is a
*sequential* data structure — there is no depth buffer anywhere in Driver 2, so
the image is correct only if primitives land in list order — and it is walked by
two cores at once without a single lock in the pixel path.

| | before | after |
|---|---|---|
| in-game frame rate | 13.5 fps | 16.8 fps |
| rasterisation, core 0 | 47 ms | 29 ms |
| rasterisation, core 1 | — | 32 ms |
| game logic + software GTE (core 0) | 27 ms | 27 ms |
| panel present | 0 ms (already on core 1) | 0 ms |

The frame waits for both bands, so the critical path is the *slower* of the two —
32 ms — not their sum. That is why the numbers do not halve: 47 → 32 ms of raster,
plus 27 ms of unchanged game logic, is 59 ms per frame. (The same 47 ms plus the
same 27 ms is the 74 ms frame this change started from.)

---

## 1. One list, two bands

Both cores execute the same function over the same pointer:

```c
// PsyX_GPU.cpp:848
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
    SR_WalkOT(p);
```

`SR_WalkOT` (`PsyX_GPU.cpp:865`) is the original OT walk, split out of
`ParsePrimitivesLinkedList` for exactly this purpose. Nothing about it is
parallel: it chases `nextPrim()` down the linked list and calls `ParsePrimitive`
on every packet. The worker task runs the identical call:

```c
// esp_raster.cpp:51
static void rasterWorker(void*)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        SR_WalkOT(sWalkPtr);
        xTaskNotifyGive(sMaster);
    }
}
```

The only difference between the two executions is a horizontal band of VRAM that
each core is allowed to write. That clamp lives in one place, at the top of every
primitive:

```c
// PsyX_SoftRas.cpp:102
static void loadEnvState()
{
#ifdef ESP32_PORT
    const int slot = xPortGetCoreID();
#else
    const int slot = 0;
#endif
    st.slot = slot;

    st.clipX0 = activeDrawEnv.clip.x;
    st.clipY0 = activeDrawEnv.clip.y;
    st.clipX1 = activeDrawEnv.clip.x + activeDrawEnv.clip.w - 1;
    st.clipY1 = activeDrawEnv.clip.y + activeDrawEnv.clip.h - 1;
    ...
    // Band restriction. The two bands partition the WHOLE of VRAM rather than
    // the clip rect, so a DR_AREA packet arriving mid-walk can never move
    // pixels outside their union.
    if (st.clipY0 < g_srBandY0[slot]) st.clipY0 = g_srBandY0[slot];
    if (st.clipY1 > g_srBandY1[slot]) st.clipY1 = g_srBandY1[slot];
```

Every write path in the rasteriser — `rasterTri`, `drawSprite`, `drawLine` —
already respected `st.clipY0 / st.clipY1`, because that is how the PSX scissor
rectangle is implemented. Narrowing those two fields is therefore the *entire*
mechanism. No pixel loop was modified.

### Why the painter's ordering survives

Two properties combine:

1. **Disjointness.** The bands are `[0, split-1]` and `[split, 511]`. They do not
   overlap, so every VRAM word is inside exactly one core's band and is written
   by exactly one core. There is no shared pixel and therefore no data race on
   the framebuffer, and no need for atomics or a lock in the inner loop.
2. **Order within a band.** Each core visits primitives in strict OT order. So
   for any two primitives *A* before *B* in the list that both touch a pixel *p*,
   the core that owns *p* draws *A* then *B*. Painter's ordering is preserved
   pixel by pixel, which is all it ever meant.

What is *not* preserved is any global notion of "the frame is half drawn". At a
given instant core 0 may be at OT index 0x40 while core 1 is at 0x1C0. That is
harmless for pixels, and it is exactly what makes the three pieces of shared
state in section 3 dangerous.

### Why the bands partition all of VRAM, not the clip rect

```c
// esp_raster.cpp:91
// Partition ALL of VRAM, not just the clip rect, so a DR_AREA packet
// arriving mid-walk cannot move pixels outside the union of the bands.
g_srBandY0[0] = 0;         g_srBandY1[0] = sSplitY - 1;
g_srBandY0[1] = sSplitY;   g_srBandY1[1] = VRAM_HEIGHT - 1;
```

A `DR_AREA` packet embedded in the ordering table rewrites `clip` mid-walk
(`PsyX_GPU.cpp:1514-1529`); Driver 2 uses this for the overlay and the Tanner
window. If the bands had been derived from the clip rect that was live at fork
time, a `DR_AREA` moving the scissor downwards would let core 0 draw into rows
core 1 also owns. Anchoring the partition to VRAM itself — `0 … 511` — makes the
invariant unconditional: whatever the packet stream does to `clip`, the clamp
still lands each core inside its own half.

```
VRAM, 1024 x 512, 16 bpp
     0                     320                                        1023
   0 +-----------------------+-------------------------------------------+
     |  draw buffer A        |                                           |
     |  clip = 0,0,320,240   |   texture pages / CLUTs                   |
     |  ....... sky .......  |                                           |
 120 |=======================|===========  split line  ==================|   <- core 0 band ends
     |  .. road, traffic ..  |                                           |
 240 |                       |                                           |
 256 |  draw buffer B        |                                           |
     |  clip = 0,256,320,240 |                       sun sample @1008,456|
 511 +-----------------------+-------------------------------------------+
       core 0 band = [0, split-1]        core 1 band = [split, 511]
```

---

## 2. The fork/join, in one diagram

```
core 0 — "game" task, prio 5, core 0      core 1 — "raster1" task, prio 6, core 1
────────────────────────────────────      ──────────────────────────────────────
DrawOTag(ot)                              (blocked in ulTaskNotifyTake)
 ParsePrimitivesLinkedList(p, 0)
  SoftRas_ForkOT(p)
    g_drawEnv[1] = g_drawEnv[0]
    bands, counters reset
    xTaskNotifyGive(sWorker) ─────────▶   wakes, SR_WalkOT(sWalkPtr)
  SR_WalkOT(p)                            band [split, 511]
   band [0, split-1]
     POLY_GT3 ... TILE ... SPRT             POLY_GT3 ... TILE ... SPRT
     DR_TPAGE -> g_drawEnv[0]               DR_TPAGE -> g_drawEnv[1]
                                       ·
     DR_LOAD (CLUT upload)                  DR_LOAD (CLUT upload)
      SR_BarrierEnter()                      SR_BarrierEnter()
        ulTaskNotifyTake ◀───notify─────       xTaskNotifyGive(sMaster)  "parked"
        returns 1                              ulTaskNotifyTake  (blocks)
      LoadImage(&rect, p)                          ·
      SR_BarrierExit() ────notify──────▶       wakes, returns 0
     ... rest of list ...                   ... rest of list ...
                                            xTaskNotifyGive(sMaster)
  SoftRas_JoinOT()  ◀────notify──────────   (walk finished, sleeps again)
    memcmp(g_drawEnv[0], g_drawEnv[1])
    rebalance sSplitY from g_srBandPx[]
 return
```

The worker is created once, at boot, and then parked forever on a notification —
there is no task creation per frame:

```c
// esp_raster.cpp:60
void SoftRas_ForkInit(void)
{
    // Internal stack: an ISR runs on the interrupted task's stack on Xtensa,
    // and the panel DMA completion handler is IRAM-resident precisely so it can
    // fire with the flash cache disabled — at which point a PSRAM stack is
    // unreachable.
    // 6 KB: measured high-water use is under 2 KB, and the rest is headroom for
    // an interrupt, which on Xtensa runs on the interrupted task's stack.
    if (xTaskCreatePinnedToCore(rasterWorker, "raster1", 6144, NULL, 6,
                                &sWorker, 1) != pdPASS) {
```

Two tasks now live on core 1: this worker at priority 6 and the panel present
task at priority 4 (`display_esplcd.c:260`). They coexist because the present is
transfer-bound — 320x240x2 bytes at 80 MHz SPI is 15.4 ms, and it measured 17 ms,
so the present task is blocked on a DMA semaphore for essentially all of it. When
both are runnable the raster worker outranks it and takes the CPU.

The fork is skipped for degenerate frames:

```c
// esp_raster.cpp:77
int SoftRas_ForkEnabled(void)
{
    // Not worth forking a band smaller than a couple of DMA chunks.
    return g_srForkEnabled && sWorker && g_drawEnv[0].clip.h >= 32;
}
```

Note also `ParsePrimitivesLinkedList(p, 1)` — the single-primitive path used by
`DrawPrim` — never forks. It is a handful of primitives; the rendezvous would
cost more than it saved.

---

## 3. The three pieces of state that had to become per-core

The walk is a pure function of the OT bytes with three exceptions. Each one is a
place where the *later* position of one core would have leaked into the *earlier*
work of the other.

### 3.1 The per-primitive render state, `SRState`

`SRState` (`PsyX_SoftRas.cpp:61`) is the decoded form of everything a primitive
needs: scissor box, drawing offset, texture page base and format, blend rate, the
16-entry CLUT copied out of VRAM, the texture-window masks, and the band slot. It
is rebuilt from scratch for every primitive by `loadEnvState()` + `loadTpage()` +
`loadClut()`.

It used to be one file-scope object. That is fine for one walker and catastrophic
for two: core 0 calls `loadTpage(p->tpage)` for a road quad, and before it reaches
its pixel loop core 1 calls `loadClut(p->clut)` for a car polygon two hundred OT
entries later. Core 0 then rasterises the road with the car's palette. The failure
is not subtle — whole surfaces take the wrong colours, and it changes from frame
to frame with scheduling.

```c
// PsyX_SoftRas.cpp:83
// Thread-local: the raster worker on the other core builds its own primitive
// state while the game task is mid-primitive. rasterTri copies this into a
// local for the pixel loop, so the TLS indirection is paid once per primitive.
#ifdef ESP32_PORT
static __thread SRState st;
#else
static SRState st;
#endif
```

The cost of `__thread` is a concern in a rasteriser, but it is not paid per pixel.
`rasterTri` already took a local snapshot for an unrelated reason — writes through
`vram[]` may alias a file-scope object, so the compiler was reloading every field
on every pixel:

```c
// PsyX_SoftRas.cpp:427
// Local copy of the render state. `vram` writes below would otherwise be
// assumed to alias the `st` global, forcing a reload of every field per
// pixel. Also hoist the framebuffer base for the same reason.
const SRState s = st;
```

So the TLS lookup happens once per triangle, and `st.slot` — which the balancer
needs — rides along inside the snapshot: `g_srBandPx[s.slot] += spanPx`
(`PsyX_SoftRas.cpp:620`).

### 3.2 The draw environment, `g_drawEnv[2]`

This is the subtler one, because the draw environment looks like *input*. It is
not. The ordering table contains packets that mutate it as the walk proceeds:

| packet | subtype | what it writes | source |
|---|---|---|---|
| `DR_TPAGE` | `0xE1` | `tpage`, `dtd`, `dfe` | `PsyX_GPU.cpp:1499` |
| `DR_TWIN` | `0xE2` | `tw.x/y/w/h` (texture window) | `PsyX_GPU.cpp:1507` |
| `DR_AREA` | `0xE3`/`0xE4` | `clip.x/y/w/h` | `PsyX_GPU.cpp:1516` |
| `DR_OFFSET` | `0xE5` | `ofs[0]`, `ofs[1]` | `PsyX_GPU.cpp:1533` |
| null `POLY_FT3` | — | `tpage` (SCE tpage hack) | `PsyX_SoftRas.cpp:801` |

That last one deserves a mention because it is easy to miss. Driver 2 emits
`POLY_FT3` primitives with all vertices set to `-1` purely to change the texture
page — the hardware discards the triangle but latches the `tpage` field:

```c
// PsyX_SoftRas.cpp:800
POLY_FT3* p = (POLY_FT3*)tag;
if (p->x0 == -1 && p->y0 == -1 && p->x1 == -1) // SCE null-poly tpage hack
{
    loadTpage(p->tpage);
    activeDrawEnv.tpage = p->tpage;
    return 7;
}
```

and the whole TILE/SPRT family reads it back rather than carrying its own:

```c
// PsyX_SoftRas.cpp:934
loadTpage(activeDrawEnv.tpage);
```

So the draw environment is a running accumulator over the packet stream, and a
single shared copy has exactly the same defect as a shared `SRState`, but with a
longer reach: the wrong `tpage` persists until the next `DR_TPAGE`, so one leak
mis-textures every sprite until the end of that sub-list. A leaked `clip` is worse
still — it would let a core write outside its band.

The fix is one array and one macro:

```c
// PsyX_GPU.cpp:31
DRAWENV g_drawEnv[2];
```

```c
// PsyX_GPU.h:31
#ifdef ESP32_PORT
#include "freertos/FreeRTOS.h"
// Cheap: one rsr.prid. Read once per primitive, never per pixel. The game task
// is pinned to core 0 and the raster worker to core 1, so the core id IS the slot.
#define activeDrawEnv (g_drawEnv[xPortGetCoreID()])
#else
#define activeDrawEnv (g_drawEnv[0])
#endif
```

Because the game task is pinned to core 0 and the worker to core 1, the core id
*is* the slot — no thread-local storage, no index to pass down, and every existing
`activeDrawEnv` reference in PsyCross keeps working unmodified, including on PC
where the macro degenerates to `g_drawEnv[0]`. Counted over the PsyCross tree,
that is 81 occurrences across 64 lines — 45 in `PsyX_GPU.cpp`, 19 in `PsyX_SoftRas.cpp`, 12 in
`LIBGPU.C`, 5 in `PsyX_main.cpp` — plus the two macro definitions in
`PsyX_GPU.h`. None of them changed.

Both slots are seeded identically at the fork:

```c
// esp_raster.cpp:96
g_drawEnv[1] = g_drawEnv[0];              // identical seed for the replay
```

and from there each core replays the same packet stream into its own slot. Since
the stream is identical and the packet handlers are pure, the two slots must end
bit-identical. That is not an assumption — it is checked at every join (section 6).

`xPortGetCoreID()` compiles to a single `rsr.prid` plus a mask. It is evaluated
once per primitive inside `loadEnvState`, and inside the `DR_*` handlers which run
a handful of times per frame. It never appears in a pixel loop.

### 3.3 The profiling counters

Less dramatic, but it would have invalidated every measurement in this document.
The cycle counter on Xtensa is **per-CPU**: `esp_cpu_get_cycle_count()` reads the
local core's `CCOUNT`. Two cores accumulating into one variable produce a number
that is neither a sum of wall-clock time nor a critical path, plus a torn
read-modify-write on every increment.

```c
// PsyX_GPU.cpp:1595
#ifdef SOFTRAS_PROFILE
    extern unsigned g_dbgRasCycles[2];
    const unsigned c0 = esp_cpu_get_cycle_count();
    primLength = SoftRas_Primitive(polyTag);
    g_dbgRasCycles[xPortGetCoreID()] += esp_cpu_get_cycle_count() - c0;
#else
```

The reporting side then treats the two bands as what they are — parallel work
whose cost to the frame is the maximum, not the sum:

```c
// esp_host.cpp:95
// cycles -> ms at 240 MHz, averaged over the 60 frames. The frame
// waits for both cores, so the critical path is the slower band.
const unsigned long r0 = g_dbgRasCycles[0] / 240000 / 60;
const unsigned long r1 = g_dbgRasCycles[1] / 240000 / 60;
...
         (unsigned long)(ms / 60) - (r0 > r1 ? r0 : r1),
```

`g_srBandPx[2]` (`PsyX_SoftRas.cpp:100`) is split for the same reason, and here it
is not only a measurement — it is the balancer's input, so a torn increment would
feed back into the geometry of the next frame.

Two counters were deliberately left shared and unsynchronised: `g_dbgRasTris`
(`PsyX_SoftRas.cpp:400`) and `g_dbgRasBboxPx` (`PsyX_SoftRas.cpp:622`). They are
frame-total diagnostics printed once a second, they are never read back into the
render, and a lost increment on a collision changes a rounded triangle count.
Worth stating plainly: these two numbers are approximate under the fork. The
cycles-per-pixel figure derived from `g_dbgRasBboxPx` inherits that same small
uncertainty.

---

## 4. The barrier: side effects that reach outside the band

The band clamp makes the *rasteriser* safe, because every rasteriser write goes
through `st.clipY0/clipY1`. It does nothing for primitives that bypass the pixel
path entirely. `SoftRas_Primitive` returns `-1` for anything outside `0x20…0x70`
(`PsyX_SoftRas.cpp:781`), and those packets fall through to PsyCross's own
handlers, which operate on raw VRAM with no notion of a band.

Two of them occur during gameplay.

### 4.1 The sun sample — a VRAM-to-VRAM blit that reads the framebuffer

`DrawLensFlare` in `Game/C/sky.c` measures how bright the sun is by copying a
16x10 rectangle *out of the framebuffer it is currently drawing* into a scratch
corner of VRAM, then reading it back with `StoreImage` on the following frame:

```c
// Game/C/sky.c:464
RECT16 sun_source = { 1008, 456, 16, 10 };
...
// Game/C/sky.c:664
source.x = sun_pers_conv_position.vx;
source.y = sun_pers_conv_position.vy + last->disp.disp.y;

sample_sun = (DR_MOVE*)current->primptr;
SetDrawMove(sample_sun, &source, 1008, 456);

addPrim(current->ot + 0x20, sample_sun);
```

It is added at OT index `0x20`, i.e. in the middle of the list, after the sky has
been drawn and while both cores are still writing. Three separate problems:

- it **reads** pixels the other core is concurrently writing;
- it **ignores the clip rectangle** — `MoveImage` goes straight to
  `GR_CopyVRAM` (`LIBGPU.C:131`), and the destination `(1008, 456)` is nowhere
  near the draw buffer, so a band clamp would not even apply to it;
- it stages every row through **one file-scope buffer**, so two cores executing
  it simultaneously would interleave into the same array:

```c
// esp_stubs.cpp:85
void GR_CopyVRAM(unsigned short* src, int x, int y, int w, int h, int dst_x, int dst_y)
{
    extern unsigned short vram[VRAM_WIDTH * VRAM_HEIGHT];
    static unsigned short row[VRAM_WIDTH];   // staging: the rects may overlap
```

### 4.2 Palette cycling — CLUT uploads

`Game/C/objanim.c` animates water and neon by rotating palette entries and
re-uploading the CLUT through a `DR_LOAD` packet, up to twelve per frame:

```c
// Game/C/objanim.c:183
DR_LOAD cyclecluts[12];
...
// Game/C/objanim.c:289
SetDrawLoad(&cyclecluts[i], &vram);
addPrim(current->ot, &cyclecluts[i]);
```

Running the upload on both cores writes the same bytes, so it is idempotent in
the abstract — but the destination is a CLUT that the *other* core may be reading
at that instant, and `loadClut` pulls sixteen entries one at a time
(`PsyX_SoftRas.cpp:158`). Two cores writing the same rows while a third read is
in flight is exactly the classic torn read: half the palette from before the
rotation, half from after, for one primitive. It shows as a one-frame colour flash
on animated surfaces.

### 4.3 The rendezvous

Both sites are wrapped the same way:

```c
// PsyX_GPU.cpp:1628
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

```c
// PsyX_GPU.cpp:1673
#ifdef ESP32_PORT
    // Uploads a texture page or CLUT that a later primitive samples.
    // Running it on both cores writes the same bytes, but it would tear
    // under the other core's in-flight fetches. (objanim.c cycles CLUTs
    // this way, up to a dozen per frame.)
    if (SR_BarrierEnter())
        LoadImage(&rect, (u_long*)drload->p);
    SR_BarrierExit();
#else
```

and the rendezvous itself is thirty lines of FreeRTOS notifications:

```c
// esp_raster.cpp:131
// Rendezvous for a primitive whose side effect must happen exactly once.
// Both cores reach every barrier, in the same order, because they walk the same
// list — so the notification counts always pair up.
int SR_BarrierEnter(void)
{
    if (!sWorkerActive)
        return 1;                                   // single core: just do it

    if (xPortGetCoreID() == 1) {
        xTaskNotifyGive(sMaster);                   // "parked"
        if (!ulTaskNotifyTake(pdTRUE, SR_WAIT_TICKS))
            ESP_LOGE(TAG, "barrier resume timed out");
        return 0;
    }

    if (!ulTaskNotifyTake(pdTRUE, SR_WAIT_TICKS))   // wait for the worker to park
        ESP_LOGE(TAG, "barrier park timed out");
    return 1;
}

void SR_BarrierExit(void)
{
    if (sWorkerActive && xPortGetCoreID() == 0)
        xTaskNotifyGive(sWorker);
}
```

Semantics: the worker announces that it has stopped and blocks; core 0 waits for
that announcement, performs the operation alone with the worker provably idle,
then releases it. `SR_BarrierEnter` returns non-zero on exactly one core, so the
side effect happens exactly once.

Order does not matter. If core 0 reaches the barrier first it blocks in
`ulTaskNotifyTake` until the worker parks; if the worker gets there first its
`xTaskNotifyGive` increments a counter that core 0's take consumes immediately.
FreeRTOS task notifications are counting, not edge-triggered, so there is no
lost-wakeup window in either direction.

### Why the notification counts always pair up

This matters more than it looks, because **the barrier and the join share the same
notification slot** — both use the default index on `sMaster`. A single stray or
missing `give` would not merely stall the barrier; it would make `SoftRas_JoinOT`
return while the worker is still walking, and core 0 would present a half-drawn
frame and start mutating `g_drawEnv[0]` underneath a live reader. So the argument
has to be airtight:

1. **Both cores reach every barrier.** The `SR_BarrierEnter` calls sit in
   `ParsePrimitive`'s handler for primitive types `0x00/0x1` and `0xA0`. Those are
   reached *before* any band or clip test — `SoftRas_Primitive` returns `-1` for
   every packet outside `0x20…0x70` and the switch runs unconditionally
   (`PsyX_SoftRas.cpp:781`, `PsyX_GPU.cpp:1608`). No band-dependent branch can
   skip a barrier on one core and not the other.
2. **In the same order.** Both cores traverse the same immutable linked list from
   the same head pointer with the same deterministic control flow, so the *n*-th
   barrier core 0 reaches is the *n*-th barrier core 1 reaches. There is nothing
   to match up by identity.
3. **Each barrier moves exactly one notification each way.** Worker: one `give` to
   master (park), one `take` on itself (resume). Master: one `take` (consume the
   park), one `give` to the worker (release). No path gives twice or takes twice.
4. **The window is bounded.** `sWorkerActive` is set in `SoftRas_ForkOT` before
   the worker is woken and cleared in `SoftRas_JoinOT` after it has finished, so
   both cores' barrier calls fall strictly inside it and both take the two-core
   path. Outside the window, `SR_BarrierEnter` returns 1 immediately and the
   operation is performed inline exactly as on the single-core build.

Therefore the counter at the join is zero-plus-one — the one `give` from
`rasterWorker` at the end of the walk — and `SoftRas_JoinOT` cannot return early.

The measured cost is around thirteen rendezvous per frame (one sun blit, up to
twelve CLUT uploads), against roughly 2300–3100 primitives while driving.
Negligible, and it does not scale with scene complexity.

---

## 5. The load balancer

A fixed 50/50 split would be wrong for this game specifically. The top of a
Driver 2 frame is sky — a handful of large, cheap, flat-ish quads — and the bottom
is road, traffic and the player's car, which is where the texture fetches and the
minification filter live. Splitting VRAM down the middle gives core 1 far more
work than core 0.

So the split line moves. `g_srBandPx[slot]` counts the span pixels each core
actually touched (`PsyX_SoftRas.cpp:620`, accumulated per triangle and published
once, not per pixel), and the join uses it:

```c
// esp_raster.cpp:119
// Move the split toward whichever core drew fewer pixels. Capped at 32
// lines per frame, and skipped on near-empty frames so it cannot oscillate.
const int p0 = (int)g_srBandPx[0], p1 = (int)g_srBandPx[1];
if (p0 + p1 > 4096) {
    sSplitY -= (p0 - p1) * 32 / (p0 + p1);
    const int y0 = g_drawEnv[0].clip.y;
    const int y1 = y0 + g_drawEnv[0].clip.h - 1;
    if (sSplitY < y0 + 8) sSplitY = y0 + 8;
    if (sSplitY > y1 - 8) sSplitY = y1 - 8;
}
```

Reading it term by term:

- **Direction.** If core 0 (the top band) drew more pixels, `p0 - p1 > 0`, so
  `sSplitY` decreases and the top band shrinks. The busier core gives lines away.
- **Magnitude.** The step is proportional to the *relative* imbalance
  `(p0-p1)/(p0+p1)`, which is bounded by ±1, so the step is bounded by ±32 lines
  per frame. A proportional controller with a hard slew limit. It cannot chase a
  one-frame explosion (an explosion, a cut-scene camera) into a bad split.
- **Deadband.** `p0 + p1 > 4096` — about 4k covered pixels, 5% of a 320x240
  frame — skips the update on near-empty frames: a fade, a loading screen, a
  paused menu. Dividing by a near-zero denominator there would produce a full ±32
  swing from noise and oscillate.
- **Clamp.** Never within 8 lines of either edge of the clip rect, so neither
  band can collapse to nothing.

And the re-seed at the top of the fork:

```c
// esp_raster.cpp:85
const int y0 = g_drawEnv[0].clip.y;
const int y1 = y0 + g_drawEnv[0].clip.h - 1;

if (sSplitY <= y0 + 8 || sSplitY >= y1 - 8)      // first frame or mode change
    sSplitY = y0 + g_drawEnv[0].clip.h / 2;
```

This catches the first frame after boot and a video-mode change — Driver 2's
frontend draws 640x512 and the game 320x240, and a split line inherited across
that transition would be meaningless.

The reported result is 29 ms against 32 ms — roughly a 10% residual imbalance,
against the ~40% a fixed midpoint would leave.

### An honest caveat about the balancer

Reading the code carefully, the per-frame correction does **not** in fact carry
over during gameplay, and the reason is worth writing down because it is a nice
example of an implicit assumption that the game violates.

`sSplitY` is stored as an **absolute VRAM line**. Driver 2 double-buffers
*vertically*: the two draw environments are 256 lines apart,

```c
// Game/C/system.c:832
void InitaliseDrawEnv(DB* pBuff, int x, int y, int w, int h)
{
	SetDefDrawEnv(&pBuff[0].draw, x, y, w, h);
	SetDefDrawEnv(&pBuff[1].draw, x, y + 256, w, h);
```

and `SwapDrawBuffers` puts the current one into the live environment immediately
before walking the OT (`Game/C/system.c:651`, `PutDrawEnv` then `DrawOTag`). With
`NTSC_VERSION`, `SCREEN_H` is 240, so `clip.y` alternates 0 and 256 every single
frame.

Trace it: a frame drawn into buffer A has `y0 = 0`, `y1 = 239`, and settles
`sSplitY` at, say, 128. The next frame draws into buffer B, `y0 = 256`,
`y1 = 495`, and the guard `sSplitY <= y0 + 8` fires (128 ≤ 264) — so the split is
re-seeded to the geometric centre, 376. The frame after that draws into A again,
376 ≥ 231 fires, re-seeded to 120. The adaptation is thrown away every frame, and
the split actually used in game is always the midpoint of the current buffer.

This is a *balance* issue only, never a correctness one: the bands still partition
all 512 lines of VRAM and the invariant of section 1 is untouched. It also means
the measured 29 vs 32 ms is what the **midpoint** split yields on this content,
not what the controller converged to — which incidentally says the sky/road
imbalance is milder than the design assumed. The minimal fix is to store the split
as an offset from `clip.y` rather than an absolute line, and re-seed only when
`clip.h` changes. That is untested here and is stated as a code reading, not as a
measurement.

---

## 6. The safety net

Three independent mechanisms, on the principle that a concurrency bug in a
renderer should show up as a logged glitch, never as a hang and never as silent
corruption.

### 6.1 The draw-environment divergence canary

Section 3.2 argued that both slots must end bit-identical. The join checks it
rather than trusting it:

```c
// esp_raster.cpp:105
void SoftRas_JoinOT(void)
{
    if (!ulTaskNotifyTake(pdTRUE, SR_WAIT_TICKS))
        ESP_LOGE(TAG, "join timed out - worker stuck");
    sWorkerActive = 0;

    // Canary: the per-core draw-environment replay must land bit-identical.
    // If it ever does not, the two cores saw different primitive streams and
    // the frame is suspect - resync so the error cannot accumulate.
    if (memcmp(&g_drawEnv[0], &g_drawEnv[1], sizeof(DRAWENV)) != 0) {
        g_srEnvDiverge++;
        g_drawEnv[1] = g_drawEnv[0];
    }
```

`DRAWENV` is about 40 bytes; one `memcmp` per frame is free. What makes it a good
canary is its *scope*: the environment is a fold over the entire packet stream, so
it detects any difference in what the two cores parsed — a packet visited by one
core and not the other, a length mis-parse that desynchronises the walk, a torn
read of the list. It is a checksum of the traversal, not of the pixels.

On mismatch it resyncs `g_drawEnv[1]` from `g_drawEnv[0]` so a single bad frame
cannot poison the next one, and increments a counter that is printed in the
per-second profile line:

```c
// esp_host.cpp:100
ESP_LOGI(TAG, "  raster c0 %lu + c1 %lu ms/f  present %lu  other %lu | "
              "%lu tris/f  %lu kpx/f  %lu cyc/px  diverge %d",
```

Observed value: zero, across every session since the change landed.

### 6.2 Timeouts everywhere, no infinite waits

```c
// esp_raster.cpp:47
// Long enough that a real stall is unmistakable, short enough that a
// divergence bug degrades to a glitched frame plus a log line, not a hang.
#define SR_WAIT_TICKS pdMS_TO_TICKS(200)
```

Every wait in the fork path — the join, the barrier park, the barrier resume —
uses it, checks the return value, logs, and **continues**. 200 ms is about three
frames at 16.8 fps, so it cannot fire on ordinary scheduling jitter, and a real
deadlock costs the player a visible hitch rather than a watchdog reset.

The degradation path is deliberate rather than accidental: if the join times out,
`sWorkerActive` is cleared while the worker may still be walking. A barrier the
worker then reaches sees `sWorkerActive == 0` and takes the single-core branch,
performing the operation itself instead of blocking forever on a master that has
moved on. The frame is wrong; the game keeps running; the log says why.

The one wait deliberately left unbounded is the worker's outer
`ulTaskNotifyTake(pdTRUE, portMAX_DELAY)` — that is the idle park, not a wait on
another core.

### 6.3 The kill switch

```c
// esp_raster.cpp:44
int g_srForkEnabled = 1;                     // runtime kill switch
```

Checked first in `SoftRas_ForkEnabled()`. Setting it to zero — from a debugger, or
a one-line patch — reverts to the exact single-core path, because the band arrays
default to the whole of VRAM:

```c
// PsyX_SoftRas.cpp:92
// Horizontal band each core may draw into, and the span pixels it drew.
// The defaults span all of VRAM, so with the raster worker disabled this is
// bit-identical to single-core rendering.
...
// PsyX_SoftRas.cpp:98
int g_srBandY0[2] = { 0, 0 };
int g_srBandY1[2] = { VRAM_HEIGHT - 1, VRAM_HEIGHT - 1 };
```

This is what made the verification in section 7 an A/B on one binary rather than
two builds. Note it is a variable, not a console key — the serial console binds
`p`/`v`/`t`/`r` for dumps and `f` for the texture filter (`esp_platform.cpp:306`),
but nothing for the fork.

### 6.4 Stack

The worker task is created with **6144 bytes** of **internal** SRAM, on a board
where only ~20 KB of internal RAM is free after the app. Both halves of that
sentence are deliberate:

- *Internal*, because on Xtensa an interrupt runs on the interrupted task's stack,
  and the panel's DMA-completion handler is IRAM-resident precisely so it can fire
  while the flash cache is disabled — at which moment a PSRAM stack is unreachable
  and the fault is a crash inside an ISR.
- *6144 bytes*, against a measured high-water free figure of **4376 bytes** —
  i.e. under 1.8 KB ever actually used. The rasteriser does not recurse; the
  headroom is for the interrupt frame. The figure is reported every second so the
  margin can be re-checked rather than assumed:

```c
// esp_raster.cpp:157
unsigned SoftRas_WorkerStackFree(void)
{
    return sWorker ? uxTaskGetStackHighWaterMark(sWorker) : 0;
}
```

For contrast, the game task's stack is 96 KB and lives in PSRAM
(`esp_main.cpp:115`) because the PSX code recurses deeply through the renderer.

---

## 7. Verification: the frame is byte-identical to the single-core render

The argument in sections 1–4 is a proof sketch, and proof sketches about
concurrency are worth exactly as much as the test that backs them.

**Method.** The port has a serial screen dump built for debugging without a
camera: pressing `p` on the console makes the next present emit the displayed
region of VRAM as ASCII hex.

```c
// esp_platform.cpp:59
// [dbg] 'p' over serial dumps the display area as 160x120 RGB332 hex, so the
// on-panel image can be reconstructed on the host without a camera.
```

```c
// esp_platform.cpp:129
} else if (sDumpRequest) {               // 'p': what is on the panel
    sDumpRequest = 0;
    dumpScreen(&vram[dy * VRAM_WIDTH + dx], VRAM_WIDTH, dw, dh, 160, 120);
}
```

160 x 120 = **19200 pixels**. Dump a static scene with the fork enabled, flip
`g_srForkEnabled` to 0, dump the same scene again, diff the two dumps: identical,
0 of 19200 pixels different.

**Why this single test is worth so much.** It rules out three distinct failure
modes simultaneously, and they are the three that matter:

| failure mode | how it would appear in the dump |
|---|---|
| a seam at the split line | a horizontal discontinuity at row `sSplitY`, or a row of unwritten pixels |
| primitives lost | any polygon whose band was mis-assigned would be missing over a large area |
| primitives drawn twice | a semi-transparent primitive blended twice reads visibly darker/brighter; palette leaks (section 3.1/3.2) recolour whole surfaces |

All three are large-area, high-contrast effects. That is what makes a decimated
dump a strong test rather than a weak one.

**Its limits, stated plainly.** The dump is not a `memcmp` of VRAM. It samples
every second pixel on each axis (160x120 out of 320x240) and quantises BGR555 down
to RGB332 (`esp_platform.cpp:73-79`). A one-least-significant-bit difference, or a
difference confined to odd rows and columns, would not show. It is a static scene,
so it does not exercise every path — in particular it does not exercise the sun
blit in every sun position. It is a very strong smoke test for the failure modes
above, and it is not a proof of bit-exactness at every pixel of every frame.

The `diverge` counter of section 6.1 is the continuous complement to this one-shot
test: it runs on every frame of every session, including while driving, and covers
traversal divergence rather than pixels.

---

## 8. Where the time goes now

At 16.8 fps, with the per-second profile line:

```
16.8 fps  320x240  ~2400 prims/frame
  raster c0 29 + c1 32 ms/f  present 0  other 27 | ... cyc/px  diverge 0
  worker stack free 4376 B
```

- **32 ms** — the slower band. This is the critical path and the next target.
- **27 ms** — game logic plus the software GTE, entirely on core 0, entirely
  serial. This is now the floor: even a free rasteriser would only reach ~37 fps.
- **0 ms** — the panel present, moved to core 1 earlier and overlapped with the
  next frame's work; the game task never waits on SPI DMA.

The obvious next question — why not band-render into internal SRAM to avoid PSRAM
framebuffer writes — was asked and answered with a measurement before this work:
redirecting the framebuffer to internal SRAM changed the cost from 96 to 95 cycles
per pixel. The inner loop is ALU-bound, not memory-bound, so a band-rendering
redesign was rejected. That is also why splitting the *screen* rather than the
*primitive stream* was the right axis to parallelise: it needs no extra memory
traffic, no per-core buffers, and no merge pass.

---

## 9. Open points and trade-offs

1. **The balancer's state is discarded every frame in gameplay** (section 5).
   `sSplitY` is an absolute VRAM line and Driver 2 alternates draw buffers between
   VRAM y=0 and y=256, so the "first frame or mode change" re-seed fires on every
   frame. Balance only, not correctness. Fix: store the split relative to
   `clip.y`. Code reading, not measured.
2. **The bands are never restored to full VRAM after a join.** `g_srBandY0/Y1` are
   written only in `SoftRas_ForkOT` (`esp_raster.cpp:93`), while the clamp in
   `loadEnvState` is unconditional. The single-primitive path — `DrawPrim`, used by
   `Game/C/overmap.c` for the in-game map and `Game/C/loadview.c` for the loading
   screen — does not fork, so it inherits core 0's band from the previous forked
   frame and would be clipped to roughly the top half of the draw buffer. Neither
   screen is reached in the free demo data this port runs, which is very likely why
   it has not been observed. Fix: reset both bands to `0 … VRAM_HEIGHT-1` at the
   end of `SoftRas_JoinOT`. Code reading, not measured.
3. **`g_dbgRasTris` and `g_dbgRasBboxPx` are raced deliberately** (section 3.3), so
   the triangle count and the cycles-per-pixel figure derived from them carry a
   small unquantified error under the fork.
4. **Verification is a decimated, quantised dump**, not a full-VRAM comparison
   (section 7).
5. **The barrier list is closed by observation, not by construction.** Two
   primitive types are known to have out-of-band side effects and are wrapped. Any
   future primitive that writes VRAM outside the rasteriser — a new `DR_LOAD` site,
   a `StoreImage` moved into the OT — must be wrapped too, and nothing in the build
   enforces that. The `diverge` counter would not catch it, because such a
   primitive does not touch `DRAWENV`.
