# Debugging an MCU port without a debugger

Every non-trivial bug in this port was found without a debugger. There was no
JTAG probe attached, no `gdb`, no breakpoints, no watch window, no screenshot
button, no `printf` cheap enough to call per pixel. The board's only channel to
the outside world was a serial console, and its only output device was a
320x240 panel you have to physically look at.

That constraint is not unusual for microcontroller work, and it is worth being
precise about what it actually costs you:

- **You cannot inspect state.** You can only inspect state you decided in
  advance to print, before you knew what the bug was.
- **You cannot look at the framebuffer.** The panel shows it, but you cannot
  zoom, diff two frames, or read a pixel value.
- **You cannot afford to observe.** A `printf` per primitive at 1200
  primitives a frame is thousands of console writes a second; the measurement
  destroys the thing being measured.
- **A crash gives you a hex address**, not a stack trace with function names.
- **The turnaround is a flash cycle.** Adding one `printf` costs a rebuild and
  a reflash, roughly a minute. This alone pushes you toward tools that answer
  a *class* of question rather than one instance of it.

So the tools in this document are not incidental. They are what replaced the
debugger, and they are the reason the port went from a black screen to
16.8 fps of playable gameplay. Each section says what the tool is, shows the
real code, and says which bug or which decision it actually produced.

---

## 1. The console: what goes where

The firmware's console is configured in `esp32_port/sdkconfig`:

```
CONFIG_ESP_CONSOLE_UART_DEFAULT=y
CONFIG_ESP_CONSOLE_UART_NUM=0
CONFIG_ESP_CONSOLE_UART_BAUDRATE=115200
CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y
```

Primary output is UART0 at 115200; the USB-Serial-JTAG peripheral is the
*secondary* console, so `printf` and `ESP_LOGI` appear on both. That matters
because everything in this document is output, and output is free to duplicate.

Input is different. `esp_input_init()` installs the USB-Serial-JTAG driver
directly (`esp_platform.cpp:247-251`) and `esp_input_poll()` reads from it
(`esp_platform.cpp:285`). So **commands must be sent to the USB-C port**, the
one the S3's native USB controller owns — the same port `drive.ps1` opens as
`COM6`. On that path the baud rate is a formality; it is USB CDC underneath.

There is exactly one input path into the running game, and it is shared between
the emulated pad and the debug commands. `esp_input_poll()` demultiplexes them
(`esp_platform.cpp:278-321`): `=` starts a four-hex-digit pad word, single
letters are either pad taps or debug commands. That sharing is deliberate — you
want to be *playing* when you take a dump, because the interesting frames are
the ones you drove into.

---

## 2. The serial dump commands

Four one-character commands make the board describe its own video memory in
ASCII. They exist because there is no other way to see what the rasterizer
produced: the panel shows you the final image, but a photograph of a 2.8" LCD
does not let you compare a pixel against what you expected.

| Key | What it dumps | Output size | Format |
|-----|---------------|-------------|--------|
| `p` | The area currently being displayed, rescaled | 160 x 120 | RGB332, 2 hex digits/pixel |
| `v` | The whole 1024 x 512 VRAM, half scale | 512 x 256 | RGB332, 2 hex digits/pixel |
| `t` | One texture page, decoded through its CLUT | 128 x 128 | RGB332, 2 hex digits/pixel |
| `r` | Raw VRAM words at x=448, 64 words wide | 64 x 256 | 16-bit hex words, 4 digits each |

They are dispatched at the top of `SoftRas_PresentESP()`
(`esp_platform.cpp:102-132`), i.e. on the game task at end of frame, after the
rasterizer has finished and before the panel flush. That placement is the whole
point: you are looking at a *complete, consistent* frame, not a torn one, and
for `p` you are looking at exactly the buffer that is about to be transmitted.

### 2.1 Wire format

The dumper is 24 lines (`esp_platform.cpp:63-86`):

```c
static void dumpScreen(const uint16_t* src, int stride, int sw, int sh, int ow, int oh)
{
    const uint32_t xStep = ((uint32_t)sw << 16) / ow;
    const uint32_t yStep = ((uint32_t)sh << 16) / oh;
    printf("\n#DUMP %d %d\n", ow, oh);
    for (int y = 0; y < oh; y++) {
        const uint16_t* s = src + (((uint32_t)y * yStep) >> 16) * stride;
        char line[1025];
        uint32_t sx = 0;
        for (int x = 0; x < ow; x++, sx += xStep) {
            const uint16_t c = s[sx >> 16];             // BGR555
            const uint8_t p = (uint8_t)(((c & 0x1F) >> 2) << 5      // r -> 3 bits
                                      | (((c >> 5) & 0x1F) >> 2) << 2  // g -> 3
                                      | (((c >> 10) & 0x1F) >> 3));    // b -> 2
            static const char hex[] = "0123456789abcdef";
            line[x * 2]     = hex[p >> 4];
            line[x * 2 + 1] = hex[p & 15];
        }
        line[ow * 2] = 0;
        printf("%s\n", line);
        vTaskDelay(1);   // let the console drain
    }
    printf("#ENDDUMP\n");
}
```

Four design decisions in there are worth naming, because they are what make the
thing usable rather than a nice idea:

**Hex, not binary.** The console is shared with `ESP_LOGI`, which emits ANSI
colour escapes and can interleave at any moment. A binary blob would be
corrupted silently. Hex is self-describing: a row is valid if and only if it is
exactly `w * 2` hex characters, so a host-side parser can reject anything else
and *tell you it did*. Costs 2x the bytes; buys you a stream you can trust.

**RGB332, not 555.** One byte per pixel halves the transfer and, more
importantly, halves the wall-clock time of the dump. 3-3-2 loses precision but
keeps everything you use a screen dump for: is the geometry in the right place,
is the texture the right texture, is the sky above the road. When you need exact
values you use `r`, which is lossless.

**Nearest-neighbour rescale in 16.16 fixed point.** The `xStep`/`yStep`
formulation means one dumper handles all four commands and all video modes.
Driver 2 switches resolution at runtime — the frontend draws 640x512, the game
draws 320x240 (`esp_platform.cpp:91-100`) — so a fixed-size dump would have
been wrong half the time.

**`vTaskDelay(1)` per row.** The USB-JTAG driver's TX buffer is 1024 bytes
(`esp_platform.cpp:248`). A 1024-character `v` row fills it exactly; without
yielding, `printf` blocks inside the game task with the console driver unable to
drain, and at 1 ms tick (`CONFIG_FREERTOS_HZ=1000`) the yield is also the pacing
mechanism. The cost is that a dump freezes the game for one tick per row:

| Command | Rows | Bytes | Approx. stall |
|---------|------|-------|---------------|
| `p` | 120 | ~38 KB | ~120 ms |
| `t` | 128 | ~33 KB | ~128 ms |
| `r` | 256 | ~66 KB | ~256 ms |
| `v` | 256 | ~262 KB | ~256 ms |

That is a visible hitch, not a hang. It is acceptable precisely because the
dump is triggered manually, never per frame.

The `r` command has its own header and does not go through `dumpScreen`
(`esp_platform.cpp:105-122`), because it is not an image — it is a hex listing
of 16-bit words:

```c
printf("\n#RAW 448 0 64 256\n");
```

The four numbers are x, y, width in **words**, height in rows. x=448 is
`(448 / 64) = 7`, so this is texture page slot 7 as the GPU addresses it. In a
4bpp page each word holds four texels, so 64 words is a full 256-texel-wide
page. Use `r` when you need the actual bits: was the tpage uploaded at all, is
it zeros, is the CLUT row where you think it is.

### 2.2 Turning a capture into a PNG

This script is the other half of the tool and is genuinely worth keeping. It
handles all four commands, tolerates interleaved log lines, and depends on
nothing outside the standard library — it writes the PNG itself, which is about
fifteen lines of `zlib` and `struct`.

```python
#!/usr/bin/env python3
"""dump2png.py - turn a Driver-2-on-ESP32-S3 serial capture into PNG files.

    python dump2png.py capture.txt

Writes dump000.png, dump001.png, ... one per #DUMP/#RAW block found.
Log lines interleaved with the dump are ignored: only lines that are exactly
the expected length and made entirely of hex digits are accepted as rows.
"""
import re, struct, sys, zlib


def write_png(path, w, h, rgb):
    raw = b"".join(b"\x00" + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))
    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


L3 = [i * 255 // 7 for i in range(8)]     # 3-bit channel -> 8-bit
L2 = [i * 255 // 3 for i in range(4)]     # 2-bit channel -> 8-bit


def row_rgb332(line, w):
    """'p' / 'v' / 't': two hex digits per pixel, packed RRRGGGBB."""
    out = bytearray()
    for x in range(w):
        p = int(line[x * 2:x * 2 + 2], 16)
        out += bytes((L3[p >> 5], L3[(p >> 2) & 7], L2[p & 3]))
    return out


def row_bgr555(line, w):
    """'r': four hex digits per 16-bit VRAM word, r | g<<5 | b<<10."""
    out = bytearray()
    for x in range(w):
        c = int(line[x * 4:x * 4 + 4], 16)
        out += bytes(((c & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3))
    return out


HEX = re.compile(r"^[0-9a-fA-F]+$")


def main(path):
    n = 0
    it = iter(open(path, "r", errors="replace").read().splitlines())
    for line in it:
        line = line.strip()
        m = re.match(r"^#DUMP (\d+) (\d+)$", line)
        raw = re.match(r"^#RAW (\d+) (\d+) (\d+) (\d+)$", line)
        if m:
            w, h, per, conv, end = int(m.group(1)), int(m.group(2)), 2, row_rgb332, "#ENDDUMP"
        elif raw:
            w, h, per, conv, end = int(raw.group(3)), int(raw.group(4)), 4, row_bgr555, "#ENDRAW"
        else:
            continue

        rows = []
        for line in it:
            line = line.strip()
            if line.startswith(end):
                break
            if len(line) == w * per and HEX.match(line):
                rows.append(conv(line, w))
        if len(rows) != h:
            print("block %d: got %d of %d rows (lost to log interleave)" % (n, len(rows), h))
            rows += [bytearray(w * 3)] * (h - len(rows))
        out = "dump%03d.png" % n
        write_png(out, w, h, bytearray(b"".join(rows)))
        print("%s  %dx%d" % (out, w, h))
        n += 1
    if n == 0:
        print("no #DUMP/#RAW block found")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "capture.txt")
```

Two notes on the reconstruction, since getting them wrong produces a
plausible-but-wrong image, which is the worst outcome for a debugging tool:

- **Channel order.** PSX VRAM is BGR555 with **red in the low bits**:
  `r | g<<5 | b<<10` (`PsyX_SoftRas.cpp:8`). The `r` decoder above takes
  `c & 31` as red. If you assume the usual RGB555 you get a blue/red-swapped
  image that still looks like a scene, and you will chase a phantom. The
  firmware's own panel path had to learn the same lesson the hard way — see
  section 9.
- **RGB332 expansion.** The packing is `RRRGGGBB`. Expanding a 3-bit channel by
  `<< 5` clips the white point (max 224, never 255) and tints everything dark;
  `i * 255 // 7` replicates properly. The tables `L3`/`L2` do that.

The `row_bgr555` path renders raw VRAM words as if they were 15bpp pixels. For
a 15bpp region that is exactly right. For a 4bpp texture page it is *not* an
image of the texture — it is four texels crammed into one pixel's worth of bits,
and it will look like noise. That is expected, and it is why the next tool
exists.

---

## 3. The frame profiler

`SOFTRAS_PROFILE` is defined unconditionally for this project
(`esp32_port/main/CMakeLists.txt:69-74`):

```cmake
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    NTSC_VERSION RELEASE ESP32_PORT
    USE_PGXP=0 USE_EXTENDED_PRIM_POINTERS=1
    PSYX_NO_RENDERER=1
    SOFTRAS_PROFILE=1
)
```

It adds four counters and four measurement points — raster cycles
(`PsyX_GPU.cpp:1595`), present cycles (`esp_platform.cpp:146` and `153`),
triangles (`PsyX_SoftRas.cpp:399`) and covered pixels
(`PsyX_SoftRas.cpp:621`) — plus the report itself (`esp_host.cpp:94`). The
counters live in `esp_platform.cpp:52-57`:

```c
#ifdef SOFTRAS_PROFILE
// C++ linkage: matches the `extern unsigned` declarations in the PsyCross files
// per core: the cycle counter is per-CPU, and the split also shows band balance
unsigned g_dbgRasCycles[2] = { 0, 0 };
unsigned g_dbgRasBboxPx = 0, g_dbgRasTris = 0, g_dbgPresentCycles = 0;
#endif
```

Rasterizer time is taken around the whole primitive, in `PsyX_GPU.cpp:1595-1599`:

```c
#ifdef SOFTRAS_PROFILE
    extern unsigned g_dbgRasCycles[2];
    const unsigned c0 = esp_cpu_get_cycle_count();
    primLength = SoftRas_Primitive(polyTag);
    g_dbgRasCycles[xPortGetCoreID()] += esp_cpu_get_cycle_count() - c0;
#else
    primLength = SoftRas_Primitive(polyTag);
#endif
```

`esp_cpu_get_cycle_count()` reads the Xtensa `CCOUNT` register — a handful of
cycles, and the accumulator is indexed by core id because **CCOUNT is per-CPU**
and the two cores are rasterizing simultaneously into different bands. Merging
them into one counter would have produced a number that is neither core's time
and no one's wall clock.

Triangle and pixel counts are accumulated inside the rasterizer itself
(`PsyX_SoftRas.cpp:399-401` and `621-623`), the latter once per triangle rather
than once per pixel:

```c
    g_srBandPx[s.slot] += spanPx;   // drives the band balancer at the join
#ifdef SOFTRAS_PROFILE
    { extern unsigned g_dbgRasBboxPx; g_dbgRasBboxPx += spanPx; }
#endif
```

> One honesty note about the name: `g_dbgRasBboxPx` accumulates `spanPx`, which
> is the sum of `(xe - xs + 1)` over the rows actually filled — **covered**
> pixels, not bounding-box pixels. The name is a leftover from an earlier
> version that measured the bounding box. Every "cycles per pixel" number in
> these documents is therefore per covered pixel, which is the meaningful one.

### 3.1 Reading the output

The report is printed every 60 frames from `PsyX_EndScene()`
(`esp_host.cpp:82-118`):

```c
ESP_LOGI(TAG, "%d.%d fps  %dx%d  %lu prims/frame", 60000 / ms, (600000 / ms) % 10,
         activeDispEnv.disp.w, activeDispEnv.disp.h,
         (unsigned long)(g_dbgSoftRasPrims - prims0) / 60);
#ifdef SOFTRAS_PROFILE
// cycles -> ms at 240 MHz, averaged over the 60 frames. The frame
// waits for both cores, so the critical path is the slower band.
const unsigned long r0 = g_dbgRasCycles[0] / 240000 / 60;
const unsigned long r1 = g_dbgRasCycles[1] / 240000 / 60;
const unsigned long tot = g_dbgRasCycles[0] + g_dbgRasCycles[1];
ESP_LOGI(TAG, "  raster c0 %lu + c1 %lu ms/f  present %lu  other %lu | "
              "%lu tris/f  %lu kpx/f  %lu cyc/px  diverge %d", ...);
ESP_LOGI(TAG, "  worker stack free %u B", SoftRas_WorkerStackFree());
```

Field by field:

| Field | Meaning | What it tells you |
|-------|---------|-------------------|
| `fps` | 60 frames / elapsed ms | The only number that matters at the end |
| `WxH` | `activeDispEnv.disp` | Which video mode — 320x240 in game, 640x512 in the frontend |
| `prims/frame` | primitives the rasterizer consumed | Scene complexity. ~1200 stationary, 2300-3100 while driving |
| `raster c0 / c1` | ms per frame per core | **Band balance.** The frame waits for both, so the cost is the max |
| `present` | ms per frame in the panel flush | 0 once it moved to core 1; ~17 before |
| `other` | `frame_ms - max(c0, c1)` | Game logic + software GTE + spooling |
| `tris/f` | triangles submitted | Quads count twice |
| `kpx/f` | thousands of covered pixels | Overdraw shows up here, not in `tris/f` |
| `cyc/px` | `(c0 + c1) / covered_px` | **The rasterizer's efficiency, isolated from scene size** |
| `diverge` | per-core draw-env mismatches | Must be 0. See below |
| `worker stack free` | high-water headroom, bytes | Must stay well above 0 |

The two derived numbers are the ones you actually optimise against.

**`other` is computed by subtraction, not by measurement.** It is
`ms/60 - max(r0, r1)`. That is a deliberate choice: it means anything you forgot
to instrument still shows up somewhere, instead of vanishing. When present time
moved to core 1, `present` went to ~0 and `other` did *not* grow by 17 ms —
which is how the change was confirmed to be a real win rather than a
relabelling.

**`cyc/px` is the metric that survives scene changes.** Frame rate depends on
where the car is pointing; cycles per covered pixel does not. It is what the
rasterizer rewrite was measured against, and it went **221 -> 68 cycles per
covered pixel** — the change that took the port from unplayable to playable.
Optimising against fps alone would have been unreadable noise, because driving
five metres changes the primitive count by a factor of two.

**`diverge` is a correctness canary, not a performance number.** Both cores
replay the same `DR_TPAGE` / `DR_TWIN` / `DR_AREA` packet stream into their own
`g_drawEnv[]` slot, so those two structures must end each frame bit-identical.
The check is at the join (`esp_raster.cpp:111-117`):

```c
    // Canary: the per-core draw-environment replay must land bit-identical.
    // If it ever does not, the two cores saw different primitive streams and
    // the frame is suspect - resync so the error cannot accumulate.
    if (memcmp(&g_drawEnv[0], &g_drawEnv[1], sizeof(DRAWENV)) != 0) {
        g_srEnvDiverge++;
        g_drawEnv[1] = g_drawEnv[0];
    }
```

This is the pattern worth stealing: when you parallelise something by having two
workers redundantly replay the same state machine, **assert that their copies
agree** and print the count. It is nearly free (one `memcmp` per frame), it
converts a class of race condition from "occasional weird pixels" into a number
on the console, and it self-heals so a single glitch cannot compound. Observed
value on this port: zero.

`worker stack free` is the same idea for a different failure. The raster worker
is created with **6144 bytes** of internal SRAM (`esp_raster.cpp:68`), and the
measured high-water **free** figure is **4376 bytes** — so the deepest observed
use is about 1.7 KB and the rest is headroom. On Xtensa an interrupt runs on the
interrupted task's stack, so
the headroom is not optional, and stack overflow on this platform presents as a
corrupted neighbour rather than a clean fault. Printing the high-water mark
every 60 frames turns a silent memory-corruption bug into a visibly shrinking
number.

---

## 4. Compile-time ablation: attributing cost by deletion

This is the most transferable technique in this document, so it is worth stating
as a method rather than as two flags.

**The problem.** You have an inner loop that costs 68 cycles per pixel. It does
a texture fetch, a palette lookup, a transparency test and a framebuffer write.
A sampling profiler would tell you which instructions retire — but there is no
sampling profiler here, and on an in-order core with a shared cache and external
memory, instruction counts do not tell you where the *stalls* are anyway.

**The method.** Delete one part of the loop at compile time, keep everything
else — including the loop structure, the iteration count and the surrounding
code — and measure `cyc/px` again. The difference is that part's cost,
*including* its cache effects, which is what you cannot get any other way. The
build renders garbage; that is fine, because you are reading a number off the
console, not looking at the screen.

Two such switches exist.

### 4.1 `SOFTRAS_NOTEX` — what does the texture fetch cost?

`PsyX_SoftRas.cpp:220-227`:

```c
template <int FMT>
static inline unsigned short fetchTexel(const SRState& s, int u, int v)
{
#ifdef SOFTRAS_NOTEX
    // [measurement build] no VRAM read at all: isolates texture-fetch cost
    // from framebuffer-write cost. Renders garbage on purpose.
    return (unsigned short)(0x1234 + u + v);
#endif
```

The replacement is arithmetic on values the loop already has, so the loop still
runs, still interpolates, still writes, still branches on transparency — it just
never reads texture memory. **Answer: ~34 cycles per pixel**, half the entire
inner-loop budget.

That result set the whole optimisation agenda. It is why the 16-entry CLUT is
copied into `SRState` once per primitive rather than read per pixel
(`PsyX_SoftRas.cpp:71-74`):

```c
    // 4bpp palette pulled out of VRAM once per primitive. VRAM lives in slow
    // external RAM and the framebuffer writes evict it from the data cache, so
    // reading the CLUT per pixel costs a miss; 16 entries fit in registers/stack.
    unsigned short clut16[16];
```

and it is why the texture-page base pointer is hoisted, and why the 4bpp fetch
was reduced to a single indexed word read with the 8-bit wrap folded into the
texture-window mask.

### 4.2 `SOFTRAS_FBSCRATCH` — what do the framebuffer writes cost?

Same idea, other end of the pipeline. `PsyX_SoftRas.cpp:283-287`:

```c
#ifdef SOFTRAS_FBSCRATCH
// [measurement build] one shared row in internal SRAM, used in place of the
// PSRAM framebuffer to isolate framebuffer-write cost. Renders nothing.
static unsigned short g_srScratchRow[1024];
#endif
```

and at the point the row pointer is taken (`PsyX_SoftRas.cpp:527-531`):

```c
#ifdef SOFTRAS_FBSCRATCH
            unsigned short* row = g_srScratchRow;
#else
            unsigned short* row = fb + y * VRAM_WIDTH;
#endif
```

Note the shape of the ablation: it does not remove the writes, it *redirects*
them. Every store still happens, the same number of times, with the same
addressing arithmetic — only the destination moves from 1 MB of PSRAM to 2 KB of
internal SRAM that will sit permanently in cache. The difference is purely the
memory cost of the writes.

**Answer: 96 versus 95 cycles per pixel. The framebuffer writes are free.**

That is a genuinely counter-intuitive result, and it is the most valuable single
measurement in the project. Everyone's instinct on this hardware is that writing
to external PSRAM is the bottleneck, and the standard remedy — band rendering:
rasterize into a small internal-SRAM tile, then DMA it out — is a large,
invasive redesign that would have complicated the dual-core split enormously.

**That redesign was rejected on this evidence.** The write-combining path to
PSRAM absorbs sequential 16-bit stores essentially for free; the loop is
ALU-bound, not memory-bound. The two ablations together say: spend your effort
on the fetch and the per-pixel arithmetic, not on where the pixels land. Which
is exactly what happened, and it produced the 3.3x.

### 4.3 The technique, generalised

The reason to build ablation switches rather than reason about the code:

1. **They measure stalls, not instructions.** On a core with a 64 KB data cache
   and 80 MHz external memory, the cost of an operation is dominated by whether
   its operand is resident — which depends on everything else in the loop. Only
   an end-to-end measurement of the real loop captures that.
2. **They are cheap to build and impossible to misread.** Each is under ten
   lines. The number either moves or it does not.
3. **They protect you from expensive mistakes.** The band-rendering rewrite
   would have been weeks of work justified by an assumption that turned out to
   be false. Twenty minutes of `SOFTRAS_FBSCRATCH` bought that.
4. **They must be exact about what stays.** `SOFTRAS_NOTEX` still returns a
   value that varies with `u`/`v`, so the compiler cannot fold the loop away.
   `SOFTRAS_FBSCRATCH` still writes every pixel. An ablation that accidentally
   deletes the surrounding work measures nothing and tells you it measured
   something.

A third switch, `SOFTRAS_DEBUG_CAR` (`PsyX_SoftRas.cpp:404-425`), is
instrumentation rather than ablation: it logs the texel-per-pixel ratio of
triangles landing in the region of the 320x240 viewport where the player car
sits, capped at 24 lines so it cannot flood the console. It produced the
measurement the texture filter was designed around:

```
ground, buildings, foliage   100-200 px polygons   1.2 - 1.6 texels/pixel
the player car               2-3 px polygons        11 - 23 texels/pixel
```

Two populations, three orders of magnitude apart in area, an order of magnitude
apart in compression. That is why the filter uses a 2x2 kernel in general but a
4x4 only on triangles whose **bounding box** is at most 64 pixels
(`PsyX_SoftRas.cpp:475-477`) — a rule you would never invent without the
histogram:

```c
        int side = 0;
        if (worst > 0x28000) side = 2;
        if (worst > 0x40000 && (maxX - minX + 1) * (maxY - minY + 1) <= 64) side = 4;
```

Note precisely what that test measures: `(maxX - minX + 1) * (maxY - minY + 1)`
is the area of the triangle's clipped bounding box, not its covered-pixel count.
A triangle passing the gate covers *at most* 64 pixels and usually far fewer, so
the gate is conservative — it can only ever refuse the 4x4 to a triangle that
would have been cheap, never grant it to one that is expensive. That is the
point: the bound is computed from two subtractions already in hand, before a
single span is solved, and it cannot be wrong in the direction that costs
frames.

### 4.4 Runtime ablation: the `f` key

Compile-time ablation costs a flash cycle, which makes it useless for anything
you want to judge by eye. For those, there is a runtime toggle
(`esp_platform.cpp:310-314`):

```c
            if (buf[i] == 'f') {
                g_srFilter = !g_srFilter;
                ESP_LOGI(TAG, "texture filter %s", g_srFilter ? "ON" : "off");
                continue;
            }
```

`g_srFilter` gates the minification filter (`PsyX_SoftRas.cpp:459`). Flipping it
mid-drive shows both the visual difference and, 60 frames later, the cost:
**14.5 -> 10.0 fps**.

Read that pair carefully, because it is easy to misquote. **14.5 fps is the
baseline of that particular A/B scene, not the port's headline unfiltered frame
rate, which is 16.8 fps.** The two numbers differ because they were measured in
different places: the filter toggle was exercised in a heavier part of the level.
Frame load varies enormously — roughly 1200 primitives standing still against
2300-3100 while driving — so an absolute fps figure means nothing without the
scene it came from. What the toggle *does* give you is a valid comparison,
because both halves are the same scene seconds apart, and that comparison is the
filter's cost: a third of the frame rate. (While actually driving with the filter
on, the measured rate is 5-9 fps.)

That is exactly why the runtime toggle is worth its fourteen lines. A subjective
quality/performance trade-off is much easier to settle when you can A/B it on the
panel in one second instead of two reflashes — and holding the scene fixed across
the two halves is the only thing that makes the ratio trustworthy.

The same file has `g_srForkEnabled` (`esp_raster.cpp:44`), a kill switch back to
single-core rasterization. It is honest to note that it is *not* wired to a key —
it exists for a debugger or a rebuild. Exposing it on the console the way `f` is
exposed would be a two-line change and would make single-vs-dual-core A/B as
cheap as the filter comparison.

---

## 5. `SoftRas_DebugDecodeTPage`: separating bad data from bad sampling

When a textured surface renders wrong, there are two completely different
causes, and they need completely different fixes:

- **Bad data.** The texture page never arrived in VRAM, arrived at the wrong
  address, arrived compressed, or its CLUT is somewhere else. This is a
  spooler/upload bug.
- **Bad sampling.** The data is perfect and the rasterizer is reading it wrong:
  wrong texture-window mask, wrong 4-bit nibble, wrong CLUT index, coordinates
  wrapping into the neighbouring page.

From the panel they look identical. Both give you garbage on a polygon.

The tool that separates them decodes a texture page **using the rasterizer's own
fetch function** and dumps the result as a picture
(`PsyX_SoftRas.cpp:752-772`):

```c
// [dbg] Decode one 4bpp texture page exactly the way the rasterizer samples it
// and write it as a 128x128 BGR555 image, so "bad texture data" can be told
// apart from "bad sampling".
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

The essential property is that it calls `fetchTexel<0>` — *the* fetch, the same
template instantiation the inner loop uses, not a reimplementation. A
reimplementation would be a second opinion from the same author and would agree
with the rasterizer about precisely the things most likely to be wrong. This
way, the picture answers one question exactly:

- **The dump looks like a texture** → the data is fine and the fetch is fine.
  The bug is upstream in coordinates, gradients, or the tpage/CLUT selection
  that reaches the rasterizer for that particular polygon.
- **The dump is noise** → either the page is not in VRAM at that address, or the
  fetch itself is broken. `r` distinguishes those two in one more keypress: dump
  the raw words and see whether there is structure there at all.

Note what it neutralises and what it does not. It forces `texFmt = 0` (4bpp),
clears the texture window (`twMask = ~0`, `twOff = 0`) so the window formula
cannot be the variable under test, and saves/restores `st` so a dump taken
mid-frame cannot corrupt the primitive being drawn. It samples 1:1 deliberately:
a 4bpp atlas decimated 2x is indistinguishable from noise, which would defeat
the entire purpose.

The dispatch is `esp_platform.cpp:123-128`:

```c
    } else if (sDumpRequest == 3) {          // 't': road texture page, decoded
        sDumpRequest = 0;
        static uint16_t page[128 * 128];
        // in-game scenery page, 1:1 over the region the tree quads sample
        SoftRas_DebugDecodeTPage(704, 0, 960, 294, 96, 72, page);
        dumpScreen(page, 128, 128, 128, 128, 128);
    }
```

The parameters are hard-coded: VRAM page at (704, 0) — slot 11, since
`704 / 64 = 11` — CLUT at (960, 294), windowed to the 128x128 region starting at
texel (96, 72), which is where the foliage quads sample. **This is the tool's
main limitation**: changing which page you look at is a rebuild. Making the six
parameters settable over the console (a `T x,y,cx,cy,u,v` command parsed the way
`=HHHH` already is) is the single highest-value improvement to this toolkit and
was not done.

---

## 6. Decoding panic backtraces

When the firmware crashes, ESP-IDF prints a register dump and a backtrace of raw
addresses, then reboots — `CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT=y`,
`CONFIG_ESP_SYSTEM_PANIC_REBOOT_DELAY_SECONDS=0`. It looks like this:

```
Guru Meditation Error: Core  0 panic'ed (LoadProhibited). Exception was unhandled.
Core  0 register dump:
PC      : 0x42041c60  PS      : 0x00060030  A0      : 0x82037093  A1      : 0x3fcebd50
...
Backtrace: 0x42041c60:0x3fcebd50 0x4203f680:0x3fcebd80 0x42037090:0x3fcebdb0
```

Each pair is `PC:SP`. The addresses mean nothing on their own; `addr2line`
resolves them against the ELF that produced the firmware. The exact command on
this machine:

```
C:\Espressif5.5\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin\xtensa-esp32s3-elf-addr2line.exe ^
    -pfiaC ^
    -e E:\Hardware\Driver2\REDRIVER2\esp32_port\build\driver2_esp32s3.elf ^
    0x42041c60 0x4203f680 0x42037090
```

which on this build prints:

```
0x42041c60: SoftRas_Primitive(void*) at E:/Hardware/.../PsyX_SoftRas.cpp:781
0x4203f680: loadClut(int) at E:/Hardware/.../PsyX_SoftRas.cpp:158
 (inlined by) SoftRas_DebugDecodeTPage at E:/Hardware/.../PsyX_SoftRas.cpp:761
0x42037090: SR_WalkOT at E:/Hardware/.../PsyX_GPU.cpp:880
```

The flags earn their place:

| Flag | Effect | Why it matters here |
|------|--------|---------------------|
| `-p` | one line per address, "pretty" | Otherwise function and location are on separate lines and the output is twice as long |
| `-f` | print function names | Without it you get file:line only |
| `-i` | **unwind inlined frames** | Essential. `CONFIG_COMPILER_OPTIMIZATION_PERF=y` inlines aggressively; without `-i` the crash appears in the caller and the real line is invisible. See `loadClut` inlined into `SoftRas_DebugDecodeTPage` above |
| `-a` | echo the address | Lets you match output back to backtrace entries |
| `-C` | demangle C++ | The game's `.c` files are compiled as C++ (`main/CMakeLists.txt:42-46`), so *every* game symbol is mangled |

Practical points learned the hard way:

- **The ELF must be the one that is flashed.** `build/driver2_esp32s3.elf` is
  overwritten by every build. Decoding a crash against a newer ELF gives
  confidently wrong function names. If a crash matters, copy the ELF aside
  before rebuilding.
- **Use the target-specific binary.** `xtensa-esp32s3-elf-addr2line` and
  `xtensa-esp-elf-addr2line` both exist in that `bin` directory; the generic one
  works for reading DWARF but the explicit one removes any doubt.
- **The toolchain path is not on `PATH` by default.** `build.ps1:23-30`
  constructs it with a glob over `$TOOLS\tools\xtensa-esp-elf\*\xtensa-esp-elf\bin`
  precisely so the version directory (`esp-14.2.0_20260121`) does not have to be
  hard-coded. Do the same in any wrapper script rather than pasting the version.
- **`0x420xxxxx` is flash-mapped code; `0x3fcxxxxx` and `0x3fexxxxx` are
  internal SRAM data; `0x3c0xxxxx`/`0x3d0xxxxx` is PSRAM.** Recognising the
  ranges tells you at a glance whether a bad pointer was a code address, a stack
  address, or a PSRAM buffer — often enough to identify the bug without decoding
  anything.

The crash this actually mattered for is described in the commit for keyboard
control: `ProcessCarPad` walked through a null pointer whenever the chase
target's cosmetics had never been spooled in, which the demo data guarantees, so
starting a drive crashed as soon as the rubber-banding logic ran. A
`LoadProhibited` at a small address, resolved in one `addr2line` call to a line
in the AI code.

---

## 7. Offline ground truth: parsing HAVANA.LEV in Python

Every tool so far asks the device what it thinks. That is circular if the bug is
in the device's understanding. At some point you need an **independent** answer:
what *should* be in that texture page?

Driver 2's level files are self-contained — the city geometry, the texture
pages, the palettes and the spooled regions are all in `HAVANA.LEV`. So the
ground truth can be produced on the PC, in Python, from the same bytes the
firmware memory-maps out of flash, with no game and no board involved.

### 7.1 The container

Everything below is read out of `Game/C/` (with the two record layouts coming
from `Game/engine/tset.h`), and the file offsets in this section were verified
against the real `data/DRIVER2DEMO/LEVELS/HAVANA.LEV`.

**The header.** The `.LEV` opens with a `LUMP_LUMPDESC` (type 37) header of two
ints, followed by four `(offset, size)` pairs. `system.c:922-924` reads exactly
that:

```c
        // skip LUMP type (37) and size, it's already hardcoded
        fseek(levFp, 8, SEEK_CUR);
        fread(citylumps[GameLevel], 1, sizeof(citylumps[GameLevel]), levFp);
```

The four entries are indexed by the constants in `system.h:148-151`:
`CITYLUMP_DATA1 = 0`, `CITYLUMP_TPAGE = 1`, `CITYLUMP_DATA2 = 2`,
`CITYLUMP_SPOOL = 3`. For the demo Havana file:

| Entry | Offset | Size | Sector |
|-------|--------|------|--------|
| DATA1 | 2048 | 172032 | 1 |
| TPAGE | 174080 | 378880 | 85 |
| DATA2 | 552960 | 231424 | 270 |
| SPOOL | 784384 | 5482496 | 383 |

**The lump chain.** `LoadGameLevel()` reads DATA1 and calls
`ProcessLumps(ptr + 8, ...)` (`main.c:380-414`), so DATA1 is itself wrapped in
one lump header — type 35, `LUMP_LEVELDESC` — and the chain of
`{int type, int size, payload}` starts 8 bytes in, each payload padded to 4
bytes, terminated by type 255 (`main.c:182-196`, `main.c:357-359`). The lump
type constants are the enum at `main.c:78-116`. Walking the demo file gives:

```
lump   5 (TEXTURENAMES)  5504      lump  22 (MOTIONCAPTURE) x11
lump  34 (TEXTUREINFO)   6368      lump  28 (CAR_MODELS)    104476
lump  12 (MODELNAMES)    3628      lump 255 (end)
lump  25 (PALLET)       10280
lump   2 (MAP)          10640
```

**`LUMP_TEXTUREINFO` (34)** is the one that matters, because it holds the list
of permanent texture pages. Its layout is `ProcessTextureInfo()`
(`texture.c:392-429`):

```
int   tpage_amount
int   texamount                    (immediately overwritten; ignore)
TP    tpage_position[tpage_amount + 1]     TP is 8 bytes (engine/tset.h:23-27)
repeat tpage_amount times:
    int    texamount
    TEXINF entries[texamount]              TEXINF is 8 bytes (engine/tset.h:29-34)
int    nperms
XYPAIR permlist[16]                        always 16 entries, only nperms used
int    nspecpages
XYPAIR speclist[nspecpages]
```

`permlist[i].x` is the texture page number, `permlist[i].y` its size in bytes.
Havana demo: 13 permanent pages — 0, 1, 2, 3, 4, 10, 20, 21, 35, 36, 37, 38, 39.

**The texture blocks.** `LoadPermanentTPages()` reads them from the sector
immediately after DATA1 — which is exactly `CITYLUMP_TPAGE` — advancing by
`(permlist[i].y + 2047) & -2048` per page (`texture.c:521-540`), i.e. each block
is sector-aligned. Each block is decoded by `LoadTPageAndCluts()`
(`texture.c:275-307`):

```c
	npalettes = *(int *)tpageaddress;
	tpageaddress += 4;

	for (i = 0; i < npalettes; i++)
	{
		LoadImage(cluts, (u_long*)tpageaddress);
		tpageaddress += 32;
		...
	}
	...
	decomp_asm((char*)_other_buffer, tpageaddress);
	LoadImage(&temptpage, (u_long*)_other_buffer);
```

So: one `int npalettes`, then `npalettes` CLUTs of 32 bytes each (16 entries x
15bpp), then the compressed page.

**The compression** is `decomp_asm()` (`Game/ASM/compres.c`, 23 lines), a byte
RLE that fills a fixed `0x8000`-byte buffer **backwards**:

```c
char* decomp_asm(char* dest, char* src)
{
	char* ptr = dest + 0x7fff;

	do {
		char pix = *src++;

		if ((pix & 0x80) != 0)
		{
			char p = *src++;
			do (*ptr-- = p);
			while (pix++ <= 0);
		}
		else
		{
			do (*ptr-- = *src++);
			while (pix-- != 0);
		}
	} while (ptr >= dest);

	return src;
}
```

`0x8000` bytes is 64 words x 256 rows x 2 bytes — one texture page, exactly the
`64 x 256` rect `LoadImage` then uploads. Working out the run lengths from the
`do/while` with post-increment is the fiddly part: for a literal token
`t` in `0..127` the count is `t + 1`; for a run token `t` in `128..255` (signed
value `t - 256`) the count is `258 - t`.

### 7.2 The script

Tested against `data/DRIVER2DEMO/LEVELS/HAVANA.LEV`. The self-check that the
container walk is right is that the sector-aligned block sizes sum to exactly
378880 bytes, the declared `CITYLUMP_TPAGE` size — if any offset in the chain
were wrong, that would not land on the nose.

```python
#!/usr/bin/env python3
"""levtex.py - decode a Driver 2 permanent texture page straight out of a .LEV,
without the game. Ground truth to compare a device dump against.

    python levtex.py HAVANA.LEV            # list the permanent pages
    python levtex.py HAVANA.LEV 0 0        # decode tpage 0 with palette 0
"""
import struct, sys, zlib

CDSECTOR = 2048
CITYLUMP_DATA1, CITYLUMP_TPAGE = 0, 1
LUMP_TEXTUREINFO, LUMP_END = 34, 255


def citylumps(d):
    """The .LEV opens with a LUMP_LUMPDESC (37) header of 8 bytes, followed by
    four (offset, size) pairs: DATA1, TPAGE, DATA2, SPOOL."""
    return [struct.unpack_from("<ii", d, 8 + i * 8) for i in range(4)]


def lumps(d, data1_off):
    """DATA1 is itself wrapped in one lump header (35, LUMP_LEVELDESC); the
    chain of {int type, int size, payload} starts 8 bytes in and ends at 255."""
    out, p = {}, data1_off + 8
    while True:
        t, s = struct.unpack_from("<ii", d, p)
        if t == LUMP_END:
            return out
        out[t] = (p + 8, s)
        p += 8 + ((s + 3) & ~3)


def permlist(d, off):
    """LUMP_TEXTUREINFO layout, per ProcessTextureInfo() in Game/C/texture.c."""
    tpage_amount = struct.unpack_from("<i", d, off)[0]
    p = off + 8                             # + the unused leading texamount
    p += 8 * (tpage_amount + 1)             # TP tpage_position[tpage_amount+1]
    for _ in range(tpage_amount):           # per page: int n, then TEXINF[n]
        n = struct.unpack_from("<i", d, p)[0]
        p += 4 + 8 * n
    nperms = struct.unpack_from("<i", d, p)[0]
    p += 4
    return [struct.unpack_from("<ii", d, p + 8 * i) for i in range(nperms)]


def decomp(src, off):
    """decomp_asm() from Game/ASM/compres.c: byte RLE that fills a fixed 0x8000
    byte buffer BACKWARDS. 0x8000 == 64 words x 256 rows x 2 bytes, one tpage."""
    out, ptr = bytearray(0x8000), 0x7FFF
    while ptr >= 0:
        tok = src[off]; off += 1
        if tok & 0x80:                      # run: (258 - tok) copies of one byte
            b = src[off]; off += 1
            for _ in range(258 - tok):
                if ptr < 0: break
                out[ptr] = b; ptr -= 1
        else:                               # literal: tok + 1 bytes
            for _ in range(tok + 1):
                if ptr < 0: break
                out[ptr] = src[off]; off += 1; ptr -= 1
    return bytes(out)


def blocks(d):
    """Permanent tpages sit in CITYLUMP_TPAGE, one sector-aligned block each,
    in permlist order. Each block: int npalettes, npalettes*32 bytes of CLUT,
    then the RLE-compressed page."""
    city = citylumps(d)
    perms = permlist(d, lumps(d, city[CITYLUMP_DATA1][0])[LUMP_TEXTUREINFO][0])
    off = city[CITYLUMP_TPAGE][0]
    for tp, size in perms:
        npal = struct.unpack_from("<i", d, off)[0]
        yield tp, npal, off + 4, off + 4 + npal * 32
        off += (size + CDSECTOR - 1) & ~(CDSECTOR - 1)


def png(path, w, h, rgb):
    raw = b"".join(b"\x00" + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))
    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main(argv):
    d = open(argv[1], "rb").read()
    want = int(argv[2]) if len(argv) > 2 else None
    pal = int(argv[3]) if len(argv) > 3 else 0
    if want is None:
        for tp, npal, _, _ in blocks(d):
            print("tpage %2d  %2d palettes" % (tp, npal))
        return
    for tp, npal, clut_off, data_off in blocks(d):
        if tp != want:
            continue
        clut = struct.unpack_from("<16H", d, clut_off + pal * 32)
        words = struct.unpack("<16384H", decomp(d, data_off))
        rgb = bytearray()
        for v in range(256):
            for u in range(256):          # 4bpp: 4 texels per 16-bit word
                c = clut[(words[v * 64 + (u >> 2)] >> ((u & 3) * 4)) & 0xF]
                rgb += bytes(((c & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3))
        png("tpage%d_pal%d.png" % (tp, pal), 256, 256, rgb)
        print("wrote tpage%d_pal%d.png" % (tp, pal))
        return
    print("tpage %s is not a permanent page of this level" % want)


if __name__ == "__main__":
    main(sys.argv)
```

Output on the demo data:

```
> python levtex.py data/DRIVER2DEMO/LEVELS/HAVANA.LEV
tpage  0  24 palettes
tpage  1  21 palettes
tpage  2  16 palettes
tpage  3  21 palettes
tpage  4   8 palettes
tpage 10  33 palettes
tpage 20  30 palettes
tpage 21  24 palettes
tpage 35  29 palettes
tpage 36  30 palettes
tpage 37  29 palettes
tpage 38  17 palettes
tpage 39   7 palettes
```

`tpage 0` decodes to a recognisable Havana building atlas; `tpage 20`, with its
30 palettes, is a car body atlas — which is itself a useful confirmation, since
the multiple palettes are the traffic colour variants.

### 7.3 Why this is worth the effort

The decoding chain on the device is long: flash -> `esp_fs_map` -> spooler ->
`decomp_asm` -> `LoadImage` -> VRAM -> `fetchTexel` -> pixel. A fault anywhere
in it produces the same symptom. Having an independent decode of the *same
source bytes* collapses the search:

- Python image is correct, `t` dump is noise → the fault is on the device,
  somewhere between flash and VRAM.
- Both are noise → the container parse is wrong, or the level file itself is
  not what you think it is (wrong file, truncated flash image).
- Both correct, polygons still wrong → the textures are fine; the bug is in
  coordinates, gradients or tpage selection.

It is also the only way to be sure the flashed data image is intact. The port
packs the demo set into a flash container with `make_data.py`, and "the texture
is garbage" and "the texture never got flashed" are indistinguishable from the
panel.

The obvious extension, not done: dump the device's VRAM words with `r`, run them
through this script's CLUT decode, and diff against the Python page byte for
byte. That turns "looks about right" into a number. The pieces are all here.

---

## 8. Driving the board from PowerShell

The last tool is the one that makes the others usable in a loop. `drive.ps1`
exists mainly to play the game from the PC keyboard — a serial console
transmits key *presses* only, so single characters can tap a button but never
hold a throttle — but it doubles as the automation harness.

The protocol is a stateless four-hex-digit pad word
(`esp_platform.cpp:255-262`):

```c
// A terminal only ever transmits key PRESSES, so single characters can express
// a tap but never a hold-and-release — useless for steering and throttle.
// So the host may instead send the COMPLETE button state as "=HHHH", four hex
// digits, active high, as often as it likes. It is stateless: a dropped byte
// self-corrects on the next update, and if the host goes quiet the latch times
// out and releases everything rather than leaving the throttle stuck on.

#define HOST_MASK_TIMEOUT_MS 400
```

Two properties make it good for automation, and both were designed in rather
than discovered:

- **Stateless.** Every packet carries the complete button state, so a lost byte
  costs one 20 ms frame of input and self-corrects. A press/release protocol
  would desynchronise permanently.
- **Self-releasing.** The 400 ms timeout means a script that crashes, or a
  window that loses focus, releases the pad instead of leaving the car at full
  throttle into a wall. `drive.ps1:110-113` also sends `=0000` on exit.

`drive.ps1` reads the true keyboard state through `GetAsyncKeyState` — down
*and* up, which the console cannot give you — and streams the word at 50 Hz. It
also fires the dump commands, edge-triggered (`drive.ps1:59-60`, `91-96`):

```powershell
# single-shot debug commands the firmware understands (see esp_platform.cpp)
$DUMP = @{ 0x50 = 'p'; 0x56 = 'v'; 0x54 = 't'; 0x52 = 'r' }   # P V T R
...
            # debug dumps fire once per press, not continuously
            foreach ($vk in $DUMP.Keys) {
                $down = [Kbd]::Down($vk)
                if ($down -and -not $dumpWasDown[$vk]) { $sp.Write($DUMP[$vk]) }
                $dumpWasDown[$vk] = $down
            }
```

Edge-triggering matters: `GetAsyncKeyState` polls at 50 Hz, so a held key would
otherwise queue fifty `v` dumps, which is thirteen megabytes of hex and a
minute-long freeze.

### 8.1 A capture script

For unattended runs you want something smaller than `drive.ps1` — send a
command, read until the terminator, save the transcript. This is the whole
harness:

```powershell
# capture.ps1 - send one debug command and save the block it produces.
#   .\capture.ps1 -Cmd p -Out screen.txt
param([string]$Port = "COM6", [string]$Cmd = "p", [string]$Out = "capture.txt")

$sp = New-Object System.IO.Ports.SerialPort($Port, 115200)
$sp.ReadTimeout = 200
$sp.Open()
Start-Sleep -Milliseconds 200
$sp.DiscardInBuffer()
$sp.Write($Cmd)

$sb = New-Object System.Text.StringBuilder
$deadline = (Get-Date).AddSeconds(20)
while ((Get-Date) -lt $deadline) {
    $chunk = $sp.ReadExisting()
    if ($chunk) { [void]$sb.Append($chunk) }
    if ($sb.ToString() -match '#END(DUMP|RAW)') { break }
    Start-Sleep -Milliseconds 20
}
$sp.Close()
Set-Content -Path $Out -Value $sb.ToString() -Encoding utf8
Write-Host "captured $($sb.Length) bytes -> $Out"
```

Then:

```powershell
.\capture.ps1 -Cmd p -Out screen.txt
python dump2png.py screen.txt
```

Windows-specific points that cost time if you do not know them:

- **`ReadExisting()` plus a poll loop, not `ReadLine()`.** `ReadLine` on
  `SerialPort` throws `TimeoutException` on every gap in a bursty stream, and
  the dump has a 1 ms gap per row by construction.
- **`DiscardInBuffer()` before writing.** The board logs continuously; without
  it your capture starts mid-log-line and the first data row may be corrupt.
- **Terminate on the marker, not on a timeout.** `#ENDDUMP` / `#ENDRAW` are
  there exactly so a script knows the block is complete. A fixed sleep either
  truncates a `v` dump or wastes twenty seconds on a `p`.
- **`Set-Content -Encoding utf8`.** Without it Windows PowerShell 5.1 writes the
  system ANSI codepage, which is harmless for pure hex but mangles the ANSI
  colour escapes in the surrounding log lines.
- **Opening the port resets the board on some USB bridges.** On the S3's native
  USB-JTAG it does not, which is one more reason the commands are read from
  that peripheral rather than UART0.

A worked automation loop, e.g. for the A/B on the texture filter:

```powershell
$sp = New-Object System.IO.Ports.SerialPort("COM6", 115200); $sp.Open()
foreach ($state in 1..2) {
    $sp.Write("f")                       # toggle the filter
    Start-Sleep -Seconds 3               # let two profile lines go by
    $sp.Write("p")                       # dump the panel
    Start-Sleep -Seconds 2
    Set-Content "ab_$state.txt" $sp.ReadExisting() -Encoding utf8
}
$sp.Close()
```

Both the frame-rate lines and the screen dumps land in the transcripts, so one
run gives you the cost *and* the picture for each setting.

---

## 9. What this toolkit does not do

Stated plainly, because the gaps shaped how the work went:

- **No symbolic inspection of live state.** There is no way to ask "what is
  `activeDrawEnv.clip` right now". You add a `printf` and reflash. A minimal
  memory-peek command (`m <addr> <len>` reusing the `r` hex formatting) would be
  perhaps thirty lines and would have paid for itself several times.
- **`t` is hard-coded to one texture page.** Section 5. This is the biggest
  single gap.
- **No automated golden-image regression.** The dual-core render was verified
  byte-identical to single core by sampling 19200 pixels — 0 differed — but that
  comparison was a one-off, not a test that runs on every build. With
  `capture.ps1` and `dump2png.py` in place, a "dump frame N of a scripted replay
  and diff against a stored PNG" harness is a short script away, and it is the
  obvious next thing to build.
- **No profiling of the game side.** `other` is a single subtracted number
  covering physics, AI, the software GTE and the spooler. Splitting it would
  need its own instrumentation; at 27 ms against 32 ms of rasterization it is
  now the second-largest block and will matter soon.
- **No heap or fragmentation tracking over time.** Free heap is printed once at
  startup (`esp_main.cpp:67-70`). Long sessions are not observed.
- **The dumps stop short of the panel, and that hid a real bug for months.**
  Every tool in section 2 reads the `vram[]` array directly. Nothing here
  observes the BGR555 -> RGB565 conversion in `display_esplcd.c`, which is the
  last thing that touches a pixel before it reaches the ST7789. So when that
  conversion left red in the low bits — PSX VRAM packs a pixel as
  `STP | B<<10 | G<<5 | R`, RGB565 wants red in the high bits, and the panel is
  configured `LCD_RGB_ELEMENT_ORDER_RGB` (`board_pins.h:25`) so the MADCTL
  colour-order bit is clear and swaps nothing — the board displayed the entire
  game with red and blue exchanged, an orange sky over blue asphalt, while every
  `p` and `v` dump decoded to a perfectly correct image. The defect was
  eventually found by reading the code, not by looking at the screen or at a
  dump; the fix is the `psx555toPanel()` helper at `display_esplcd.c:187-195`,
  used by both `displayFlush555` and `displayFlush555Scaled`. The general lesson
  is worth more than the bug: **a debug view that taps the pipeline upstream of a
  transform can never see a fault in that transform**, and it will reassure you
  about it every single time you look.

---

## 10. Summary

| Tool | Where | Answers |
|------|-------|---------|
| `p` / `v` / `t` / `r` dumps | `esp_platform.cpp:63-132` | What is actually in VRAM, without a camera |
| `dump2png.py` | this document | Turns a capture into an image, tolerating log interleave |
| `SOFTRAS_PROFILE` | `esp_host.cpp:94-112`, `PsyX_GPU.cpp:1595-1599` | Where the frame time goes; `cyc/px` as the scene-independent metric |
| `diverge` canary | `esp_raster.cpp:111-117` | Whether the two cores agree, every frame, for one `memcmp` |
| `worker stack free` | `esp_raster.cpp:157-160` | Stack headroom before it corrupts a neighbour |
| `SOFTRAS_NOTEX` | `PsyX_SoftRas.cpp:223-227` | Texture fetch costs ~34 cyc/px — half the loop |
| `SOFTRAS_FBSCRATCH` | `PsyX_SoftRas.cpp:283-287, 527-531` | PSRAM framebuffer writes are free (96 vs 95) — band rendering rejected |
| `SOFTRAS_DEBUG_CAR` | `PsyX_SoftRas.cpp:404-425` | Texel/pixel histogram that shaped the filter |
| `f` key | `esp_platform.cpp:310-314` | Runtime A/B of a quality/cost trade-off |
| `SoftRas_DebugDecodeTPage` | `PsyX_SoftRas.cpp:752-772` | Bad data or bad sampling |
| `addr2line -pfiaC` | section 6 | A hex backtrace into file:line, inlines included |
| `levtex.py` | section 7 | Independent ground truth straight from the `.LEV` |
| `drive.ps1` / `capture.ps1` | `esp32_port/drive.ps1`, section 8 | Driving and scripting the board over serial |

If there is one transferable idea here, it is the pairing in sections 3 and 4:
**a scene-independent metric plus compile-time ablation**. `cyc/px` gave a
number that did not move when the game did, and the ablation switches attributed
that number to specific work. Together they turned "the rasterizer is slow" into
"the texture fetch is half the cost and the framebuffer writes are free", which
is a statement you can act on — and, just as importantly, told the project *not*
to spend weeks on a band-rendering redesign that every instinct said was the
right answer.
