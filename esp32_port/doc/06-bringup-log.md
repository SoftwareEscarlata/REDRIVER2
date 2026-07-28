# Bring-up log: from a black screen to gameplay

This is the chronological account of getting Driver 2 from "the firmware links and
boots" to "you can drive it" on a Waveshare ESP32-S3-Touch-LCD-2. Eight failures,
in the order they appeared. Each one was hiding the next, so the sequence is not
padding: nothing after step *n* could be observed until step *n* was fixed.

The value here is not the fixes — most of them are two or three lines. It is the
diagnosis. There is no debugger on this board. No JTAG probe was attached, no
`gdb`, no breakpoints, no watch expressions. The only channel out of the device is
a USB-serial console, and the only channel in is the same console plus ten GPIO
buttons. Everything below was found by making the board *tell* us something over
that one wire.

The port had reached this point in commit `47f4691e` ("Driver 2 boots on the
ESP32-S3"): the firmware links, PSRAM is allocated for the PSX memory map, the
OLVL flash container is mounted at `/d`, the ST7789 comes up, and
`redriver2_main()` runs without faulting. It just did not draw anything.

Fixes 1–7 landed as `fa95b9a6` ("esp32: boot the game, fix input, and take the
panel flush off the critical path"), which took the board from a black screen to
~13.5 fps of gameplay. Fix 8 landed as `a66cde6c` ("esp32: drive from the PC
keyboard"). Dual-core rasterisation (`d6dec58d`) later took it to 16.8 fps; that
is a performance story, documented elsewhere.

---

## Summary

| # | Symptom | What actually broke | Fix location |
|---|---|---|---|
| 1 | Black panel, one frame, log goes silent | Missing data file falls into an infinite CD-retry loop | `make_data.py`, `partitions.csv` |
| 2 | Frontend appears, but cropped to its top-left quarter | Present clamped the source rect instead of rescaling it | `esp_platform.cpp`, `display_esplcd.c` |
| 3 | Stuck forever on "PLEASE WAIT" | State machine re-enters a state that can never succeed | `FEmain.c:1927`, `glaunch.c:195-204` |
| 4 | Buttons do nothing | Pad report published on the wrong hook, with its two bytes swapped | `esp_host.cpp:123-136`, `esp_platform.cpp:174-187` |
| 5 | Cross reads as permanently pressed | GPIO17 is driven low by this board | `esp_platform.cpp:239-245` |
| 6 | Crash entering the level | Spooler `malloc`s 16 MB, gets NULL, writes through it | `spool.c:1434-1445` |
| 7 | Crash on the first in-game frame | `GR_CopyVRAM` dereferenced a NULL `src` | `esp_stubs.cpp:83-113` |
| 8 | Crash a second after starting to drive | `ProcessCarPad` walks a null `carCos` | `handling.c:1330` |

---

## The instruments

Four instruments did all of the work. Three of them are still compiled into the
firmware; the fourth comes with the toolchain.

### 1. One funnel for every file open

The ESP32 build force-includes `esp_compat.h` into every game and PsyX
translation unit (`esp32_port/main/CMakeLists.txt:78`), and that header redefines
`fopen`:

```c
// esp32_port/main/esp_compat.h:23
#define fopen(name, mode) ((FILE*)esp_game_fopen((name), (mode)))
```

This exists because the game uses DOS-style relative paths (`GFX\SPLASH1N.TIM`)
that have to be resolved against the `/d` VFS. But the side effect is more useful
than the intent: **every file the game will ever ask for passes through one
function**, so a single log line gives a complete, ordered trace of the boot's
data dependencies.

```c
// esp32_port/main/esp_fs.c:267-280
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
    if (!f)
        ESP_LOGD(TAG, "missing: %s", name);
    return f;
}
```

The line is `ESP_LOGD`, so at the shipped `CONFIG_LOG_DEFAULT_LEVEL_INFO` it is
removed at compile time and costs nothing. During bring-up the level was raised
and every open — hit and miss — was printed.

### 2. A frame heartbeat

`PsyX_EndScene` is the one function the game calls once per presented frame, so it
is where the liveness probe goes (`esp_host.cpp:82-118`). Every 60 frames it
prints the frame rate, the display mode the game is currently asking for, and the
number of primitives the rasteriser consumed:

```c
ESP_LOGI(TAG, "%d.%d fps  %dx%d  %lu prims/frame", 60000 / ms,
         (600000 / ms) % 10,
         activeDispEnv.disp.w, activeDispEnv.disp.h,
         (unsigned long)(g_dbgSoftRasPrims - prims0) / 60);
```

Three separate facts from one line. If it stops printing, the game task is stuck
or dead. If it prints but the panel is wrong, the problem is in the present path.
And `activeDispEnv.disp.w/h` is what caught failure #2 directly.

### 3. Serial screen dumps

Without a camera pointed at the panel there is no record of what was displayed.
So the firmware can dump framebuffer contents as hex over the console
(`esp_platform.cpp:63-86`), reduced to 160x120 and RGB332 so that a frame is a few
kilobytes of text rather than 150 KB of binary:

```c
static void dumpScreen(const uint16_t* src, int stride, int sw, int sh, int ow, int oh)
{
    const uint32_t xStep = ((uint32_t)sw << 16) / ow;
    const uint32_t yStep = ((uint32_t)sh << 16) / oh;
    printf("\n#DUMP %d %d\n", ow, oh);
    ...
        vTaskDelay(1);   // let the console drain
```

Four dumps are wired to single keystrokes on the console
(`esp_platform.cpp:306-309`):

| key | what it dumps |
|---|---|
| `p` | the display area exactly as the panel sees it, 160x120 |
| `v` | all 1024x512 of VRAM at half scale — texture pages, CLUTs, both framebuffers |
| `t` | one texture page *decoded the way the rasteriser samples it* (4bpp indices through the CLUT) |
| `r` | raw 16-bit VRAM words of one page, as hex, for byte-level comparison against the source file |

A short Python script on the host turns `#DUMP` blocks back into PNGs. This is
slow and ugly and it answered questions nothing else could — `v` in particular
tells you instantly whether a texture is missing (VRAM is empty), wrongly uploaded
(garbage in the page), or correctly uploaded but wrongly *sampled* (page looks
right, geometry looks wrong).

The `vTaskDelay(1)` per line matters: the USB-JTAG console is installed with a
1 KB TX buffer (`esp_platform.cpp:247-251`) and drops rather than blocks when it
overflows. Without the yield the dumps come back with holes in them.

### 4. `addr2line` on the panic backtrace

Covered in its own section below, because it only becomes relevant at failure #6.

---

## 1. Black screen — the boot never gets past the splash

**Symptom.** The panel showed one frame — black — and then nothing. The serial log
printed the startup banner, the PSRAM and heap report, `gamedata: N files mounted
at 0x3c...`, `ST7789 up: 80000000 Hz SPI, 16-line chunks`, and then went silent.
The board was not resetting: no panic, no reboot banner, no watchdog. It was
simply not progressing.

The one frame is real and explainable. `redriver2_main` calls `ResetGraph(0)` at
`main.c:1821`, and PsyCross implements mode 0 as "clear and present":

```c
// src_rebuild/PsyCross/src/psx/LIBGPU.C:137-147
int ResetGraph(int mode)
{
	if (mode == 0)
	{
		// reset GPU state
		g_GPUDisabledState = 0;
		ClearImage(&activeDrawEnv.clip, 0, 0, 0);

		ClearSplits();
		PsyX_EndScene();
	}
```

So exactly one (black) present reaches the ST7789, and then execution disappears
somewhere after it.

**Diagnosis.** Raise the log level so `esp_game_fopen` prints every open. The
trace runs through `DATA\FEFONT.BNK`, `GFX\FONT2.FNT`, the language files — and
ends on:

```
missing: GFX/SPLASH1N.TIM
```

Nothing after it. So the boot is not crashing on a missing file, it is *stopping*
inside whatever handles the failure.

**Root cause.** `main.c:1885` calls `ShowHiresScreens(ScreenNames, 300, 0)` with
`NTSCScreenNames[0] = "GFX\\SPLASH1N.TIM"` (`main.c:1768`), which reaches
`FadeInHiresScreen` and `E3stuff.c:128`:

```c
LoadfileSeg(filename, (char*)_overlay_buffer, 20, 0x4ff80);
```

`LoadfileSeg` has two backends and they are *both* compiled in. `driver2.h:30-37`
sets `USE_PC_FILESYSTEM 1` for every non-PSX target and `USE_CD_FILESYSTEM 1`
unconditionally ("use always"). The PC path is tried first, and when it misses it
falls through to the CD path:

```c
// src_rebuild/Game/C/system.c:355-395
	FILE* fptr = fopen(namebuffer, "rb");
	if (fptr)
	{
		...
		return numRead;
	}
#endif // USE_PC_FILESYSTEM

#if USE_CD_FILESYSTEM
	...
	sprintf(namebuffer, "\\%s%s;1", gDataFolder, name);

	if (strcmp(currentfilename, namebuffer) != 0)
	{
		strcpy(currentfilename, namebuffer);

		while (CdSearchFile(&currentfileinfo, namebuffer) == NULL)
		{
			DoCDRetry();
		}
	}
```

On a real PlayStation that loop is correct: the disc might be spinning up, or the
lid might be open, so you retry until the file appears. On this board there is no
CD drive at all — game data is memory-mapped flash — and `CdSearchFile` is a stub
that can only ever return NULL:

```c
// esp32_port/main/esp_host.cpp:190-200
// CD — game data is memory-mapped flash, so nothing spins. The game only
// reaches these when USE_PC_FILESYSTEM misses, which cannot happen here.
...
CdlFILE* CdSearchFile(CdlFILE*, char*) { return NULL; }
```

The comment is the bug. "Which cannot happen here" was an assumption, and the
assumption was wrong the moment a file was left out of the image.

`system.c:391` is therefore an infinite loop. It is not even a polite one:
`DoCDRetry` (`system.c:248-257`) calls `VSync(3)` every tenth iteration, and
PsyCross's `VSync` does nothing at all for `mode > 0` (`LIBETC.C:43-46`, marked
`// FIXME: wait many times?`). So core 0 spins flat out at priority 5, the idle
task never runs, and — because `CONFIG_ESP_TASK_WDT_INIT=n` in
`sdkconfig.defaults` — no watchdog ever complains. A silent, permanent hang that
looks exactly like a dead board.

**Why the file was missing.** `make_data.py` packs the demo data into the flash
container and had been trimming it to fit an 11 MB partition:

```python
SKIP_DIRS  = {"GFX/HQ"}
SKIP_FILES = {"GFX/SPLASH1N.TIM"}
```

`GFX/HQ` is 4.7 MB of PC-only high-resolution font TGAs and is genuinely unused.
The splash TIM is 320 KB and looked equally disposable. It is not.

**Fix.** Ship it, and grow the partition:

```python
# esp32_port/make_data.py:23-25
SKIP_DIRS = {"GFX/HQ"}
# note: the splash TIMs must stay — the boot sequence blocks without them
SKIP_FILES = set()
```

```
# esp32_port/partitions.csv
gamedata, data, 0x40,    0x410000, 0xBF0000,
```

**And then the same thing again, further in.** With the splash present, the boot
advanced — and stopped again, this time on `SOUND/VOICES2.BLK`. `InitFrontend`
loads the menu sound bank (`FEmain.c:1844` → `LoadBankFromLump` →
`gamesnd.c:154`), which is another `LoadfileSeg`, which is the same loop. There is
no audio on this target yet and the banks had been excluded by `--no-sound` for
exactly that reason. It makes no difference: the *loader* blocks whether or not
anything will ever play the samples. The sound banks now ship too, which is why
the partition runs to the end of the 16 MB flash:

```
# demo data (OLVL container): Havana level + frontend assets + sound banks.
# The sound banks must ship even before audio exists — the frontend blocks on
# a failed VOICES2.BLK open. Sized to the end of the 16MB flash.
```

**The transferable lesson.** On a port, a missing asset is not a missing asset —
it is whatever the original code did when the disc was scratched. Read that path
before you trim anything.

---

## 2. The image appears, cropped

**Symptom.** The frontend rendered. It was recognisably Driver 2's menu — and it
was showing roughly the top-left quarter of it, at 1:1, with the rest off-screen.

**Diagnosis.** The heartbeat line printed the answer without any extra work:

```
psx: 6.1 fps  640x512  ... prims/frame
```

640x512. The panel is 320x240.

**Root cause.** The PlayStation changes video mode at runtime, and Driver 2 uses
two of them. The frontend sets up a 640x512 hi-res interlaced buffer:

```c
// src_rebuild/Game/Frontend/FEmain.c:1785-1788
	SetDefDrawEnv(&MPBuff[0][0].draw, draw_mode.x1, draw_mode.y1, 640, 512);
	SetDefDispEnv(&MPBuff[0][0].disp, draw_mode.x1, draw_mode.y1, 640, 512);
	SetDefDrawEnv(&MPBuff[0][1].draw, draw_mode.x1, draw_mode.y1, 640, 512);
	SetDefDispEnv(&MPBuff[0][1].disp, draw_mode.x1, draw_mode.y1, 640, 512);
```

while the game proper uses two 320x240 buffers stacked in VRAM:

```c
// src_rebuild/Game/C/system.c:729-730
	SetDefDispEnv(&MPBuff[0][0].disp, 0, 256, 320, SCREEN_H);
	SetDefDispEnv(&MPBuff[0][1].disp, 0, 0, 320, SCREEN_H);
```

(`SCREEN_H` is 240 for NTSC, `system.h:130`.)

The original present had been written for the second case and simply clamped:

```c
    if (dw > DISPLAY_WIDTH) dw = DISPLAY_WIDTH;
    if (dh > DISPLAY_HEIGHT) dh = DISPLAY_HEIGHT;

    displayFlush555(&vram[(dy & 511) * VRAM_WIDTH + dx], VRAM_WIDTH, dw, dh);
```

Clamping a 640x512 request to 320x240 does not scale it. It crops it.

**Fix.** Clamp only against VRAM's real bounds, and let the flush resample
(`esp_platform.cpp:88-101`):

```c
    const int dx = activeDispEnv.disp.x & (VRAM_WIDTH - 1);
    const int dy = activeDispEnv.disp.y & (VRAM_HEIGHT - 1);
    int dw = activeDispEnv.disp.w;
    int dh = activeDispEnv.disp.h;
    if (dw <= 0 || dh <= 0) return 1;
    if (dw > VRAM_WIDTH - dx)  dw = VRAM_WIDTH - dx;    // never read past VRAM
    if (dh > VRAM_HEIGHT - dy) dh = VRAM_HEIGHT - dy;
```

`displayFlush555Scaled` (`display_esplcd.c:275-305`) does 16.16 nearest-neighbour
stepping into the same 16-line DMA chunks as before:

```c
    const uint32_t xStep = ((uint32_t)sw << 16) / DISPLAY_WIDTH;
    const uint32_t yStep = ((uint32_t)sh << 16) / DISPLAY_HEIGHT;
```

At 320x240 both steps are exactly 1.0, so the in-game path pays nothing for the
generality: the inner loop is the same 555→565 conversion it was before, with an
index that happens to increment by one.

Two details worth noting. The `& (VRAM_WIDTH - 1)` / `& (VRAM_HEIGHT - 1)` on the
origin is not paranoia — the game's two buffers legitimately sit at y=0 and y=256
and the PSX wraps VRAM addressing. And the R/B channel swap between the PSX's
BGR555 and the panel's RGB565 is *not* done in this loop: it is handled by the
ST7789's MADCTL colour-order bit, so per pixel only green has to widen from 5 to 6
bits (`display_esplcd.c:193-196`).

---

## 3. Stuck forever on "please wait"

**Symptom.** The frontend now drew correctly and the menu was navigable. Left
alone for about 30 seconds it switched to a mostly-black screen with a small line
of text in the middle, and stayed there. Forever. The heartbeat kept printing — so
the game was alive and presenting frames, which ruled out another hang.

**Diagnosis.** Press `p`, decode the `#DUMP` block, look at it: the text is
`PLEASE WAIT`.

That is enough to name the code. `SetPleaseWait` is `E3stuff.c:469`; it replaces
the display environment with a 320x256 one (`E3stuff.c:479`) and prints one
centred string (`E3stuff.c:509`):

```c
	SetTextColour(128, 128, 128);
	PrintStringCentred(G_LTXT(GTXT_PleaseWait),128);
```

and the only caller on the idle path is `State_GameStart`.

**Root cause.** Two pieces of code that are each individually reasonable.

Piece one — `State_FrontEnd` starts the attract demo after 30 seconds of idle
(1800 vblanks):

```c
	if ((VSync(-1) - idle_timer) > 1800)
	{
		if (ScreenDepth == 0)
		{
			GameType = GAME_IDLEDEMO;
			gCurrentMissionNumber = gIdleReplay + 400;
			...
			SetState(STATE_GAMESTART);
```

Piece two — `State_GameStart` handles `GAME_IDLEDEMO` by loading a replay, and had
no failure branch:

```c
// src_rebuild/Game/C/glaunch.c:186-194 (before the fix)
		case GAME_IDLEDEMO:
			if (LoadAttractReplay(gCurrentMissionNumber))
			{
				...
				SetState(STATE_GAMELAUNCH);
			}
			break;
```

`LoadAttractReplay` (`replays.c:282-295`) opens `REPLAYS\ATTRACT.<n>` and returns
0 if it is not there. The DRIVER2DEMO data set ships no `REPLAYS` folder at all.

Now look at how states are dispatched:

```c
// src_rebuild/Game/C/state.c:39-61
void DoStateLoop()
{
	do
	{
		StateFn stateFn = gCurrentState;

		if (!stateFn)
			break;

		stateFn(gCurrentStateParam);
	} while (true);
}

void SetState(GameStates newState, void* param)
{
	gCurrentState = gStates[newState];
	gCurrentStateParam = param;
}
```

`gCurrentState` only changes when someone calls `SetState`. If `State_GameStart`
returns without calling it, `DoStateLoop` calls `State_GameStart` again —
immediately, forever. It re-runs `SetPleaseWait`, re-tries the same absent file,
fails the same way. The game is not hung; it is *livelocked*, which is why the
frame counter kept ticking. And because `SetPleaseWait` has already torn down the
frontend's 640x512 display env, the please-wait screen is what stays on the panel.

On a real disc the file always exists, so the missing `else` never mattered in
twenty-five years.

**Fix — two halves, deliberately.**

Don't enter a state you know will fail (`FEmain.c:1921-1927`):

```c
		char attractName[32];
		sprintf(attractName, "REPLAYS\\ATTRACT.%d", gIdleReplay + 400);

		// Only leave the menu if the attract replay really is on the disc.
		// The demo data set ships no REPLAYS folder at all, and GameStart has
		// no failure path — it would retry the missing file forever.
		if (ScreenDepth == 0 && FileExists(attractName))
```

and make the state survivable anyway (`glaunch.c:195-204`):

```c
			else
			{
				// Safety net: no attract replay available. Without this the
				// state never changes, so GameStart re-enters and retries the
				// same missing file forever. SetPleaseWait already replaced the
				// frontend's hi-res display env, so put it back.
				gInFrontend = 1;
				SetFEDrawMode();
				SetState(STATE_FRONTEND);
			}
```

The guard alone would have been enough for this data set. The `else` is what stops
the *class* of bug: any other reason `LoadAttractReplay` fails now returns to the
menu instead of wedging. Restoring `SetFEDrawMode()` is not optional — without it
you return to the frontend logic while the display env is still the 320x256 one
`SetPleaseWait` installed, and the menu renders into the wrong buffer.

Note also `FileExists` is safe to call here where `Loadfile` is not: its non-PSX
branch deliberately does not retry (`system.c:332-335`, `// don't retry or we'll
have problems`), so it returns 0 instead of falling into the loop from failure #1.

---

## 4. The pad does nothing

**Symptom.** Neither the physical buttons on header P1 nor the serial dev keys
moved the menu cursor.

**Diagnosis.** This one was read out of the game's source rather than measured,
because the question is "which API does Driver 2 actually use?" and that is a
static fact. PsyCross offers the host two ways to supply pad state, and the
obvious one is `PsyX_Pad_GetStatus`, reached from `PadGetState`
(`LIBPAD.C:63-70`). That is what the port had implemented.

Driver 2 does not read buttons that way. `ReadControllers` reads the **raw PADRAW
report buffers** that it registered with `PadInitDirect` at `pad.c:54`:

```c
// src_rebuild/Game/C/pad.c:419-440
void ReadControllers(void)
{
	int pad;

#ifndef PSX
	PsyX_UpdateInput();
#endif
	...
	pad = 0;
	do {
		PADRAW* padRaw = (PADRAW*)padbuffer[pad];

		if (padRaw->status == 0)
		{
			if (padRaw->id == 65 || padRaw->id == 115)
			{
				MapPad(pad, padRaw);
			}
```

`PadGetState` is called too — but only later in the same function, from
`HandleDualShock` (`pad.c:452` → `pad.c:265`), and only to drive the
DualShock/vibration state machine. It runs *after* `MapPad` has already consumed
the buffer. So publishing the report from `PsyX_Pad_GetStatus` meant the game read
a report that was one frame stale, and on the very first frame read an all-zero
buffer whose `id` is 0 — not 65, not 115 — which makes `ReadControllers` classify
the pad as `type = 1`, the "Incompatible controller in Port 1" case the frontend
prints at `FEmain.c:970`.

`PsyX_UpdateInput` is the correct hook: it is the one call Driver 2 makes at the
top of `ReadControllers`, before it looks at anything.

**Root cause A — wrong hook.** Moved (`esp_host.cpp:120-136`):

```c
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

`PsyX_Pad_GetStatus` now only answers the connectivity question
(`esp_host.cpp:181-185`).

**Root cause B — the bytes were swapped.** Even with the report published at the
right time, the bits were in the wrong places. `MapPad` builds a single 16-bit
word from the two report bytes:

```c
// src_rebuild/Game/C/pad.c:178-182
	mapped = Pads[pad].direct;
	buttons = ~((pData->buttons[0] << 8) | (pData->buttons[1])) & 0xffff;

	Pads[pad].direct = buttons;
	Pads[pad].dirnew = buttons & ~mapped;
```

`buttons[0]` becomes the **high** byte. The port's `PAD_*` constants had been
written to the opposite convention:

| button | port had | correct in `(buttons[0]<<8) \| buttons[1]` | what the wrong value actually meant |
|---|---|---|---|
| SELECT | `0x0001` | `0x0100` | L2 |
| START | `0x0008` | `0x0800` | R1 |
| UP | `0x0010` | `0x1000` | TRIANGLE |
| RIGHT | `0x0020` | `0x2000` | CIRCLE |
| DOWN | `0x0040` | `0x4000` | CROSS |
| LEFT | `0x0080` | `0x8000` | SQUARE |
| L1 | `0x0400` | `0x0004` | — |
| R1 | `0x0800` | `0x0008` | START |
| TRIANGLE | `0x1000` | `0x0010` | UP |
| CIRCLE | `0x2000` | `0x0020` | RIGHT |
| CROSS | `0x4000` | `0x0040` | DOWN |
| SQUARE | `0x8000` | `0x0080` | LEFT |

So the whole set was a rotation: pressing the d-pad produced face buttons and
pressing the face buttons produced d-pad directions. Fixed at
`esp_platform.cpp:171-187`, with the derivation written down so it cannot drift
again:

```c
// PSX pad bits, active low, in the word the game builds as
// (padRaw->buttons[0] << 8) | padRaw->buttons[1]  (see MapPad in pad.c).
// buttons[0] carries d-pad/start/select, buttons[1] the face and shoulder keys.
```

The same table is duplicated once more, in `drive.ps1:32-37`, because the host-side
keyboard driver builds the identical word.

---

## 5. Cross is permanently pressed

**Symptom.** With #4 fixed, the menu cursor moved — but nothing could be
*selected*. Confirm never fired.

**Diagnosis.** Log the raw mask returned by `esp_input_poll` each frame. It is
active-low; the CROSS bit was 0 with nothing touched. Unplug everything from
header P1: still 0. So it is not a stuck switch, it is the pin.

The reason a permanently-held button is *worse* than a dead one is in `MapPad`:

```c
	mapped = Pads[pad].direct;          // last frame
	buttons = ~(...) & 0xffff;          // this frame
	Pads[pad].direct = buttons;
	Pads[pad].dirnew = buttons & ~mapped;
```

`dirnew` is the rising edge — the set of buttons pressed *this frame that were not
pressed last frame*. A line that is asserted every single frame appears in
`direct` forever and in `dirnew` never. The frontend acts on new presses, so a
stuck line is not "always confirming", it is "can never confirm".

**Root cause.** `PIN_BTN_A` was mapped to GPIO17 (`board_pins.h:57`), which on the
Waveshare ESP32-S3-Touch-LCD-2 reads 0 at reset even with the ESP32's internal
pull-up enabled — the board drives it. The pin map notes it:

```c
// NOTE: IO17 measures 0 at reset even with the internal pullup enabled, so this
// board drives it. esp_input_init() detects that and disables the line; CROSS
// is still reachable on GPIO0 (BOOT) and over serial.
#define PIN_BTN_A       17  // P1 pin 5  (unusable on this board - see above)
```

**Fix.** Rather than hard-code "GPIO17 is bad", sample every button line once at
init and disable any that is already low. A button that is genuinely held down
during the first millisecond of boot is not a case worth supporting; a pin the
board drives is (`esp_platform.cpp:234-245`):

```c
    // A line that is already low at reset is not a pressed button — it is a pin
    // this board drives itself (GPIO17 on the Waveshare S3-Touch-LCD-2 has an
    // external pull-down that beats the internal pull-up). Left enabled it
    // reads as a permanently held button, which kills edge-triggered menu
    // input. Sample once and drop those lines.
    for (unsigned i = 0; i < sizeof(sButtons) / sizeof(sButtons[0]); i++) {
        const int level = gpio_get_level((gpio_num_t)sButtons[i].gpio);
        if (!level) {
            sButtonStuck |= 1u << i;
            ESP_LOGW(TAG, "button gpio%d stuck low at boot - disabled", sButtons[i].gpio);
        }
    }
```

and the poll skips them (`esp_platform.cpp:337-339`). It logs a warning, so the
next board revision — or the next person who wires a different header — finds out
in one boot instead of one afternoon. CROSS remains reachable on GPIO0 (the BOOT
button, `esp_platform.cpp:341`) and over serial.

At this point the frontend was fully usable and "TAKE A DRIVE" could be selected.

---

## Interlude: decoding a panic without a debugger

Failures 1–5 were hangs and wrong pixels. Everything from here on is a crash, so
this is where the fourth instrument arrives.

An ESP32-S3 exception prints something like this to the console:

```
Guru Meditation Error: Core  0 panic'ed (StoreProhibited). Exception was unhandled.

Core  0 register dump:
PC      : 0x4038a6b1  PS      : 0x00060830  A0      : 0x8200f3c2  A1      : 0x3d80a2f0
...
EXCVADDR: 0x00000000  LBEG    : 0x40056f28  LEND    : 0x40056f38  LCOUNT  : 0xffffffff

Backtrace: 0x4038a6ae:0x3d80a2f0 0x4200f3bf:0x3d80a310 0x42011c4d:0x3d80a340 ...
```

Two things are immediately readable without any tooling:

- **The exception class.** `StoreProhibited` = a write to an address with no write
  permission. `LoadProhibited` = a read from one. `InstrFetchProhibited` = you
  jumped somewhere insane, usually a corrupted function pointer.
- **`EXCVADDR`.** The address that was accessed. `0x00000000` is a null
  dereference and nothing else. A value like `0x00000018` is a null *struct
  pointer* with a field offset, which is even more informative — the offset tells
  you roughly which member. A plausible-looking `0x3c...`/`0x3d...` address that
  still faults usually means flash/PSRAM mapping, not a null.

The backtrace is a list of `PC:SP` pairs. Turning the PCs into source lines needs
the exact ELF that produced the running binary:

```bash
C:/Espressif5.5/tools/xtensa-esp-elf/esp-14.2.0_20260121/xtensa-esp-elf/bin/xtensa-esp-elf-addr2line.exe \
    -pfiaC -e E:/Hardware/Driver2/REDRIVER2/esp32_port/build/driver2_esp32s3.elf \
    0x4038a6ae 0x4200f3bf 0x42011c4d
```

The flags are not decoration on this project:

| flag | why it is needed here |
|---|---|
| `-e <elf>` | `esp32_port/build/driver2_esp32s3.elf` — must be from the same build as the flashed `.bin`, or every line number is fiction |
| `-f` | print the function name, not just file:line |
| `-i` | **expand inlined frames.** `sdkconfig.defaults` sets `CONFIG_COMPILER_OPTIMIZATION_PERF` (`-O2`); without `-i` a small static function shows up as its caller and you look in the wrong place |
| `-C` | **demangle.** `esp32_port/main/CMakeLists.txt:42-46` compiles all of `Game/C`, `Game/Frontend`, `Game/ASM` and PsyCross's `psx/*.C` **as C++**, exactly as upstream does. Every game symbol in the ELF is mangled; without `-C` you get `_Z13ProcessCarPadP8CAR_DATAjcc` |
| `-p` | one readable line per frame instead of two |
| `-a` | echo the address, so a frame that resolves to `??` is still identifiable |

Practical notes learned the hard way:

- **Only the first address matters most of the time.** The top frame is the fault
  site. The rest tell you how you got there, which matters only when the fault site
  is a library function like `memcpy` — in which case the *caller* is the real
  answer, and that is exactly what failures #6 and #7 looked like.
- **The backtrace can be short or say `CORRUPTED`.** The game task runs on a 96 KB
  stack allocated in PSRAM (`esp_main.cpp:112-121`), which the unwinder walks
  fine. A truncated trace on this project has generally meant the fault happened
  in a callee whose frame was already clobbered, and the `PC` + `A0` (return
  address) pair from the register dump was more trustworthy than the list.
- **`ccache` is on** (`build.ps1:46`). Rebuilding does not invalidate the ELF you
  are decoding against unless you actually changed something, which is convenient
  — but it also means you must not delete `build/` between crashing and decoding.

That is the entire workflow. It is crude compared to a debugger, but for a null
dereference it converges in about thirty seconds: exception class tells you read
or write, `EXCVADDR` tells you it is a null, and `addr2line -pfiaC` on the first
one or two PCs tells you which pointer.

---

## 6. Crash entering the level

**Symptom.** Selecting "TAKE A DRIVE" produced the loading screen and then an
immediate `StoreProhibited` panic with `EXCVADDR: 0x00000000`.

**Diagnosis.** `addr2line` on the top frames landed in `memcpy`, and one frame up
in `esp_fs.c`'s `vfs_read`:

```c
// esp32_port/main/esp_fs.c:134-146
static ssize_t vfs_read(int fd, void* dst, size_t len)
{
    ...
    memcpy(dst, src + f->pos, len);
```

A NULL `dst` handed to the VFS by `fread`. Walk up: `loadsectorsPC`
(`system.c:540-575`) does `fread(addr, CDSECTOR_SIZE, nsectors, fp)`, and `addr`
comes from `loadsectors`, and *that* comes from the spooler.

**Root cause.** `UpdateSpoolPC` reads the entire city data lump into RAM in one
go:

```c
// src_rebuild/Game/C/spool.c:1446-1450 (the PC path)
		int SpoolLumpSize;
		SpoolLumpSize = 16 * 1024 * 1024; // allocate 16 MB of RAM for spoolable data
		g_CurrentLevelSpoolData = (char*)malloc(16 * 1024 * 1024);

		loadsectors(g_CurrentLevelSpoolData, SpoolLumpOffset / CDSECTOR_SIZE, SpoolLumpSize);
```

On a PC, 16 MB is nothing. This board has 8 MB of PSRAM total, and by the time the
level loads most of it is already committed:

| block | size | where |
|---|---|---|
| PSX RAM arena | 2 MB | `system.c:120-122` (`g_allocatedMem`, `EXT_RAM_BSS_ATTR`) |
| emulated PSX VRAM | 1 MB | `esp_host.cpp:253-254` |
| two primitive tables | 640 KB | `PRIMTAB_SIZE` = `0x50000` with `USE_EXTENDED_PRIM_POINTERS=1` (`system.h:143`) |
| sound bank buffer | 512 KB | `esp_main.cpp:54` |
| frontend buffer | 384 KB | `esp_main.cpp:47` |
| overlay / other / other2 / replay | 4 × 320 KB | `esp_main.cpp:46-55` |
| two ordering tables | 128 KB | `OTSIZE` 0x2000 × 8 B (`OTTYPE` is `unsigned long long` on 32-bit, `psyx_compat.h:33`) |
| game task stack | 96 KB | `esp_main.cpp:112-121` |

That is roughly 6 MB before PsyX's own statics. A 16 MB allocation cannot succeed.
`malloc` returns NULL, the return value is never checked, and `loadsectors` writes
the file into address 0.

**Fix.** Don't copy it at all. The level file is already in memory-mapped flash —
`esp_fs_mount` maps the whole `gamedata` partition once at boot
(`esp_fs.c:220-241`) — and every consumer of `g_CurrentLevelSpoolData` only
`memcpy`s *out* of it, through a single macro:

```c
// src_rebuild/Game/C/spool.c:1460-1466, 1489
#define SPL_READ(dest, nsectors) \
		{ \
			int readSize = (nsectors)*CDSECTOR_SIZE; \
			memcpy(dest, spoolDataPtr, readSize); \
			spoolDataPtr += readSize; \
		}
...
		spoolDataPtr = g_CurrentLevelSpoolData - SpoolLumpOffset + current->sector * CDSECTOR_SIZE;
```

Nothing writes through the pointer, so it can point straight at read-only flash.
`esp_fs_map` (`esp_fs.c:256-265`) returns the mapped address of a container entry,
and biasing it by `SpoolLumpOffset` makes the existing address arithmetic above
resolve to `base + sector * 2048` exactly as before:

```c
// src_rebuild/Game/C/spool.c:1434-1445
#ifdef ESP32_PORT
		// The 16MB copy below does not fit on this target. The level file is
		// already memory-mapped from flash, so use it in place: every reader
		// below only memcpy's out of this pointer. (esp_fs_map is declared in
		// the force-included esp_compat.h.)
		const void* levelBase = esp_fs_map(g_CurrentLevelFileName, NULL);

		if (levelBase == NULL)
			return 0;

		g_CurrentLevelSpoolData = (char*)levelBase + SpoolLumpOffset;
#else
```

Cost: zero RAM, and the spool "reads" become memcpys out of the flash cache
instead of out of PSRAM — which is, if anything, faster. `HAVANA.LEV` is 6,266,880
bytes, so the whole thing would not have fitted even if the PSRAM had been free.

One consequence had to be handled: the buffer is no longer owned by the heap.

```c
// src_rebuild/Game/C/system.c:862-867
#ifndef PSX
#ifndef ESP32_PORT
	free(g_CurrentLevelSpoolData);	// on ESP32 this points into mmap'd flash
#endif
	g_CurrentLevelSpoolData = NULL;
#endif // PSX
```

Freeing a pointer into `esp_partition_mmap`'d space would corrupt the heap. The
`= NULL` stays, because that is what makes the next level re-map.

---

## 7. Crash in `MoveImage`

**Symptom.** With the spool fixed, the level loaded and the first in-game frame
crashed: `LoadProhibited`, `EXCVADDR: 0x00000000`.

**Diagnosis.** `addr2line -pfiaC` on the first PC gave `GR_CopyVRAM` in
`esp_stubs.cpp`; the next frame gave `MoveImage` in `LIBGPU.C`. A read from
address 0 inside a function whose first parameter is a pointer is not much of a
mystery after that.

**Root cause.** PsyCross's `MoveImage` passes NULL deliberately:

```c
// src_rebuild/PsyCross/src/psx/LIBGPU.C:131-135
int MoveImage(RECT16* rect, int x, int y)
{
	GR_CopyVRAM(NULL, rect->x, rect->y, rect->w, rect->h, x, y);
	return 0;
}
```

`GR_CopyVRAM` has two meanings depending on `src`. Non-NULL: copy a linear `w*h`
host image into VRAM (that is `LoadImage`, and it is what the port had
implemented — it even discarded `x`/`y` with `(void)x; (void)y;`). NULL: a
**VRAM-to-VRAM** blit, where `x`/`y` are the *source* rectangle's origin. The port
handled only the first case and read `src[yy * w + xx]` unconditionally.

Why it only showed up in game: the only `DR_MOVE` primitive this build emits comes
from the sky renderer sampling the sun out of the framebuffer
(`src_rebuild/Game/C/sky.c:667-672`):

```c
			sample_sun = (DR_MOVE*)current->primptr;
			SetDrawMove(sample_sun, &source, 1008, 456);

			addPrim(current->ot + 0x20, sample_sun);
			current->primptr += sizeof(DR_MOVE);
```

The frontend also has `DR_MOVE` primitives — `In` and `Out` in `FEmain.c:946-948`,
used to save and restore the pixels under the highlighted menu button — but those
call sites are inside `#ifdef PSX` (`FEmain.c:1492-1495`, `1533-1534`) and are not
compiled here. So the frontend never exercised the path, and the very first frame
of sky did.

**Fix.** Implement the real semantics, including overlap
(`esp_stubs.cpp:83-113`):

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
    ...
```

The row staging buffer and the direction flip are not theoretical. A
VRAM-to-VRAM move whose source and destination overlap is legal on the PSX, and
`memmove` semantics are what the hardware provides. Copying top-down through an
overlapping rect would read rows you had already overwritten.

Both axes wrap with `& (VRAM_HEIGHT - 1)` / `& (VRAM_WIDTH - 1)`, matching PSX
VRAM addressing.

Later, when rasterisation was split across both cores, this same primitive needed
a rendezvous — it *reads* framebuffer pixels that both cores are concurrently
writing, and it stages through one shared `row[]` buffer
(`PsyX_GPU.cpp:1628-1638`). That is a dual-core story, but it is worth noting that
the correctness requirement was discovered here first.

---

## 8. Crash while driving

**Symptom.** The car appeared, the world rendered, the frame counter settled — and
about a second after touching the throttle, a panic. Reproducible: it happened
every time the car started moving, never while stationary.

**Diagnosis.** `addr2line -pfiaC` on the top frame resolved to `ProcessCarPad` in
`src_rebuild/Game/C/handling.c`, inside the rubber-banding block. `EXCVADDR` was a
small non-zero value — a null struct pointer plus a member offset.

Reading around that line makes the mechanism obvious:

```c
// src_rebuild/Game/C/handling.c:1319-1335
				if (MainPlayer.playerCarId == cp->id)
					targetCarId = MainPlayer.targetCarId;
				else if (SecondPlayer.playerCarId == cp->id)
					targetCarId = SecondPlayer.targetCarId;
				else
					targetCarId = -1;

				// apply rubber banding to player car depending on distance from target car
				if (targetCarId != -1)
				{
					tp = &car_data[targetCarId];

					if (3050 < cp->ap.carCos->powerRatio)
						cp->thrust = FIXEDH(tp->ap.carCos->powerRatio * 4915);
```

**Root cause.** This is Driver 2's rubber-banding: in a chase, the player's engine
power is scaled by the *target* car's power and by the distance to it. It reads
`tp->ap.carCos`, the target's cosmetics record — a pointer into data the spooler
loads on demand.

`targetCarId != -1` proves a target *slot* has been assigned. It does not prove
anything was ever spooled into it. On the retail discs the chase target's model is
always resident by the time you can drive; the demo data set has no chase car at
all, so `car_data[targetCarId].ap.carCos` is NULL. It only fires once the car is
moving because that whole block sits under the throttle/thrust computation.

**Fix** (`handling.c:1327-1330`):

```c
				// apply rubber banding to player car depending on distance from target car
				// [A] the chase target can be a slot whose cosmetics were never
				// spooled in (the demo data has no chase car), and this walks
				// straight through the null pointer
				if (targetCarId != -1 && car_data[targetCarId].ap.carCos != NULL)
```

Skipping the block means no rubber-banding — which is exactly right, since there
is no car to rubber-band against.

**A second failure found in the same session.** Getting to the point where the car
could be driven for more than a second required a real keyboard driver, since a
serial console transmits key *presses* only and can never express a hold. The host
now streams the complete pad word as `=HHHH` at 50 Hz (`drive.ps1`), and that
immediately broke the receive path:

```c
// esp32_port/main/esp_platform.cpp:280-285
    // Drain the whole receive buffer, not a fixed nibble of it: the host sends
    // a 5-byte pad packet 50 times a second, which outruns any per-frame cap
    // and eventually backs up until the host's write blocks.
    uint8_t buf[64];
    int n;
    while ((n = usb_serial_jtag_read_bytes(buf, sizeof(buf), 0)) > 0)
```

The old code read a fixed 16 bytes per frame. At 5 bytes × 50 Hz = 250 B/s inbound
against 16 B/frame at ~14 fps = 224 B/s consumed, the 256-byte RX buffer fills in
about twenty seconds and the host's `SerialPort.Write` starts timing out. The
symptom — input works, then gradually stops — looked nothing like a buffer
problem, which is why it is worth recording: **a per-frame input cap is a bug
whenever the input rate is not bounded by the frame rate.**

The protocol design that goes with it is deliberately failure-tolerant: it is
stateless (a dropped byte self-corrects on the next 20 ms update), and the latch
expires after 400 ms (`HOST_MASK_TIMEOUT_MS`, `esp_platform.cpp:262`) so a host
that goes away releases the pad instead of leaving the throttle wedged on.

---

## What the sequence cost, and what it says

Eight failures, three distinct categories:

| category | failures | shared property |
|---|---|---|
| The original code's assumptions about its environment | 1, 3, 6, 8 | Every one is a path that could not fail on a PlayStation with a retail disc |
| Host-layer semantics implemented from the header rather than the caller | 2, 4, 7 | The signature was satisfied; the contract was not |
| Board reality | 5 | Only the schematic and a voltmeter know |

The first category is the interesting one, because it is the actual content of
"porting a decompilation". REDRIVER2 is not sloppy code — the retry loop in
`LoadfileSeg`, the missing `else` in `State_GameStart`, the 16 MB `malloc`, and
the unguarded `carCos` were all *correct* against the machine and the media they
were written for. A CD drive really does need retrying. An attract replay really
is always on the disc. A PC really does have 16 MB spare. A chase mission really
does have a chase car. The port did not break these; it removed the guarantees
they were resting on.

That is also why the fixes are shaped the way they are. Where possible they make
the failure *survivable* rather than merely absent for this data set: failure #3
got both a guard at the call site and a recovery path in the state; failure #5
detects any stuck line rather than blacklisting GPIO17; failure #1's diagnosis is
retained as a permanent `ESP_LOGD` on the one `fopen` funnel.

The diagnostic tools are all still in the tree and all still compiled in — the
dump keys, the heartbeat, the profile line. They cost a few hundred bytes and one
branch per frame, and they are the reason the next failure takes minutes instead
of an evening.

**Still open at the end of this log:** no audio (SPU → I2S), only the demo Havana
level (retail cities would come from microSD), and game logic plus the software
GTE now account for 27 ms of a 59 ms frame — nearly half — which is where the next
round of work has to go.

---

## Reference: the commits

| commit | repo | what it contains |
|---|---|---|
| `47f4691e` | REDRIVER2 | Boots without faulting; host layer, PSX memory map from PSRAM, PSRAM `.bss` for cold arrays |
| `fa95b9a6` | REDRIVER2 | Failures 1–7; frame profiling and the serial dumps; async present on core 1. Black screen → ~13.5 fps |
| `a66cde6c` | REDRIVER2 | Failure 8; `drive.ps1` and the `=HHHH` pad protocol; the RX drain fix |
| `4f049c6` | PsyCross | Rasteriser correctness and 3.3× inner loop (221 → 68 cycles/covered pixel) |
| `3750328` / `d6dec58d` | PsyCross / REDRIVER2 | Dual-core rasterisation, 13.5 → 16.8 fps, verified byte-identical |
</content>
</invoke>
