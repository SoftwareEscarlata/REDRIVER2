// Host-layer implementations for the ESP32 build: the parts of PsyX that live
// outside src/psx (main loop services, pad, SPU, CD, logging) plus the game's
// XA/FMV entry points. Everything here either forwards to esp_platform.cpp or
// is a deliberate no-op for hardware this target does not have.
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "PsyX/PsyX_public.h"
#include "PsyX/PsyX_globals.h"
#include "PsyX/PsyX_render.h"
#include "psx/libgpu.h"
// libcd.h declares CdDataCallback with redundant parens (parses as void*)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wparentheses"
#include "psx/libcd.h"
#pragma GCC diagnostic pop
#include "psx/libspu.h"

#include "display.h"

static const char* TAG = "psx";

extern "C" {

// ---------------------------------------------------------------------------
// PsyX globals normally defined by the SDL/GL host
// ---------------------------------------------------------------------------
int g_vmode = 0;                       // 0 = NTSC
int g_cfg_bilinearFiltering = 0;
GameOnTextInputHandler g_cfg_gameOnTextInput = NULL;
TextureID g_vramTexture = 0;
TextureID g_whiteTexture = 0;

// ---------------------------------------------------------------------------
// logging -> serial
// ---------------------------------------------------------------------------
static void logv(const char* level, const char* fmt, va_list ap)
{
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    // strip the trailing newline the callers include
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    if (n) ESP_LOGI(TAG, "%s%s", level, buf);
}

void PsyX_Log(const char* fmt, ...)         { va_list a; va_start(a, fmt); logv("",     fmt, a); va_end(a); }
void PsyX_Log_Info(const char* fmt, ...)    { va_list a; va_start(a, fmt); logv("",     fmt, a); va_end(a); }
void PsyX_Log_Warning(const char* fmt, ...) { va_list a; va_start(a, fmt); logv("WARN ", fmt, a); va_end(a); }
void PsyX_Log_Error(const char* fmt, ...)   { va_list a; va_start(a, fmt); logv("ERR ", fmt, a); va_end(a); }

// ---------------------------------------------------------------------------
// frame services
// ---------------------------------------------------------------------------
int SoftRas_PresentESP();               // esp_platform.cpp
uint16_t esp_input_poll();
uint32_t esp_millis();

// raw PADRAW buffers the game hands over via PadInitDirect
static u_char* sPadData[2];

char PsyX_BeginScene(void) { return 1; }

// [dbg] heartbeat: proves the game reached the frame path and shows what it
// is asking the display for
extern int g_dbgSoftRasPrims;   // esp_platform.cpp
extern DISPENV activeDispEnv;   // PsyX_GPU.cpp
extern DRAWENV g_drawEnv[2];   // this file only ever runs on core 0
#ifdef SOFTRAS_PROFILE
extern unsigned g_dbgRasCycles[2];
extern unsigned g_dbgRasBboxPx, g_dbgRasTris, g_dbgPresentCycles;
extern int g_srEnvDiverge;      // esp_raster.cpp
extern unsigned SoftRas_WorkerStackFree();
#endif

void PsyX_EndScene(void)
{
    static int frames = 0;
    static uint32_t t0 = 0, prims0 = 0;
    if ((++frames % 60) == 0) {
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (t0) {
            const uint32_t ms = now - t0;
            ESP_LOGI(TAG, "%d.%d fps  %dx%d  %lu prims/frame", 60000 / ms,
                     (600000 / ms) % 10,
                     activeDispEnv.disp.w, activeDispEnv.disp.h,
                     (unsigned long)(g_dbgSoftRasPrims - prims0) / 60);
#ifdef SOFTRAS_PROFILE
            // cycles -> ms at 240 MHz, averaged over the 60 frames. The frame
            // waits for both cores, so the critical path is the slower band.
            const unsigned long r0 = g_dbgRasCycles[0] / 240000 / 60;
            const unsigned long r1 = g_dbgRasCycles[1] / 240000 / 60;
            const unsigned long tot = g_dbgRasCycles[0] + g_dbgRasCycles[1];
            ESP_LOGI(TAG, "  raster c0 %lu + c1 %lu ms/f  present %lu  other %lu | "
                          "%lu tris/f  %lu kpx/f  %lu cyc/px  diverge %d",
                     r0, r1,
                     (unsigned long)(g_dbgPresentCycles / 240000 / 60),
                     (unsigned long)(ms / 60) - (r0 > r1 ? r0 : r1),
                     (unsigned long)(g_dbgRasTris / 60),
                     (unsigned long)(g_dbgRasBboxPx / 60 / 1000),
                     (unsigned long)(g_dbgRasBboxPx ? tot / g_dbgRasBboxPx : 0),
                     g_srEnvDiverge);
            ESP_LOGI(TAG, "  worker stack free %u B", SoftRas_WorkerStackFree());
            g_dbgRasCycles[0] = g_dbgRasCycles[1] = 0;
            g_dbgPresentCycles = g_dbgRasBboxPx = g_dbgRasTris = 0;
#endif
        }
        t0 = now;
        prims0 = g_dbgSoftRasPrims;
    }
    SoftRas_PresentESP();
}

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

void PsyX_GetScreenSize(int* w, int* h)
{
    if (w) *w = DISPLAY_WIDTH;
    if (h) *h = DISPLAY_HEIGHT;
}

void PsyX_GetPSXWidescreenMappedViewport(struct _RECT16* rect)
{
    if (!rect) return;
    rect->x = 0; rect->y = 0;
    rect->w = DISPLAY_WIDTH; rect->h = DISPLAY_HEIGHT;
}

void PsyX_SetSwapInterval(int) {}
void PsyX_EnableSwapInterval(int) {}

// The game paces itself on the vblank counter; drive it from the monotonic
// clock at 60 Hz (NTSC) instead of an interrupt thread.
int PsyX_Sys_GetVBlankCount()
{
    return (int)((uint64_t)esp_timer_get_time() * 60 / 1000000ULL);
}

int PsyX_Sys_SetVMode(int mode) { g_vmode = mode; return mode; }

void PsyX_WaitForTimestep(int count)
{
    static int lastVb = 0;
    if (count <= 0) { lastVb = PsyX_Sys_GetVBlankCount(); return; }
    int target = lastVb + count;
    while (PsyX_Sys_GetVBlankCount() < target)
        vTaskDelay(1);
    lastVb = target;
}

// ---------------------------------------------------------------------------
// pad — PsyX normally fills the game's raw report buffers from SDL
// ---------------------------------------------------------------------------
void PsyX_Pad_InitPad(int slot, u_char* padData)
{
    if (slot >= 0 && slot < 2) sPadData[slot] = padData;
}

int PsyX_Pad_GetStatus(int mtap, int slot)
{
    (void)mtap;
    return (slot == 0 && sPadData[0]) ? 1 : 0;   // report filled in PsyX_UpdateInput
}

void PsyX_Pad_Vibrate(int, int, unsigned char*, int) {}

// ---------------------------------------------------------------------------
// CD — game data is memory-mapped flash, so nothing spins. The game only
// reaches these when USE_PC_FILESYSTEM misses, which cannot happen here.
// ---------------------------------------------------------------------------
int CdInit(void) { return 1; }
int CdControlB(u_char, u_char*, u_char*) { return 1; }
int CdControlF(u_char, u_char*) { return 1; }
int CdRead(int, u_long*, int) { return 1; }
int CdReadSync(int, u_char*) { return 0; }
int CdGetSector(void*, int) { return 0; }
int CdDiskReady(int) { return CdlComplete; }
CdlFILE* CdSearchFile(CdlFILE*, char*) { return NULL; }
CdlLOC* CdIntToPos(int i, CdlLOC* p) { (void)i; return p; }
int CdPosToInt(CdlLOC*) { return 0; }
CdlCB CdReadyCallback(CdlCB) { return NULL; }
void* CdDataCallback(void (*)()) { return NULL; }   // matches libcd.h's parse

// ---------------------------------------------------------------------------
// SPU — audio comes in a later phase (I2S); voices report "idle" so the
// mixer-driven game logic keeps advancing.
// ---------------------------------------------------------------------------
int  PsyX_SPUAL_InitSound() { return 1; }
void PsyX_SPUAL_ShutdownSound() {}
int  PsyX_SPUAL_Alloc(int) { return 0; }
int  PsyX_SPUAL_InitAlloc(int, char*) { return 0; }
void PsyX_SPUAL_Free(u_int) {}
u_int PsyX_SPUAL_Write(u_char*, u_int size) { return size; }
u_int PsyX_SPUAL_SetTransferStartAddr(u_int addr) { return addr; }
void PsyX_SPUAL_GetVoiceVolume(int, short* l, short* r) { if (l) *l = 0; if (r) *r = 0; }
void PsyX_SPUAL_GetVoicePitch(int, u_short* p) { if (p) *p = 0; }
void PsyX_SPUAL_SetVoiceAttr(SpuVoiceAttr*) {}
void PsyX_SPUAL_SetKey(int, u_int) {}
int  PsyX_SPUAL_GetKeyStatus(u_int) { return 0; }
void PsyX_SPUAL_GetAllKeysStatus(char* status) { if (status) memset(status, 0, 24); }
int  PsyX_SPUAL_SetMute(int) { return 0; }
int  PsyX_SPUAL_SetReverb(int) { return 0; }
u_int PsyX_SPUAL_SetReverbVoice(int, u_int) { return 0; }
u_int PsyX_SPUAL_GetReverbVoice() { return 0; }

}  // extern "C"

// ---- C++-linkage stubs (declared in C++ headers, not extern "C") ----------
void SwitchMappings(int menu) { (void)menu; }   // one fixed mapping here

// ---------------------------------------------------------------------------
// XA speech / FMV — no audio or video decode on this target
// ---------------------------------------------------------------------------
void PrintXASubtitles(int) {}
void GetXAData(int) {}
void SetXAVolume(int) {}
void PrepareXA() {}
void PlayXA(int, int) {}
int  XAPrepared() { return 0; }
void UnprepareXA() {}
void StopXA() {}
void ResumeXA() {}
void PauseXA() {}

struct RENDER_ARGS;
int FMV_main(RENDER_ARGS*) { return 0; }


// emulated PSX VRAM (normally in PsyX_render.cpp, which is host-only here).
// 1MB in PSRAM: textures are read through the data cache.
#include "esp_attr.h"
EXT_RAM_BSS_ATTR unsigned short vram[VRAM_WIDTH * VRAM_HEIGHT];
