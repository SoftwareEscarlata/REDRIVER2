// Stubs for host subsystems not present on the ESP32 build:
// PsyX render/audio backends, SDL window/input plumbing, memory cards.
// The software rasterizer + esp_platform.cpp cover what the game actually needs.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"

#include "PsyX/PsyX_public.h"
#include "PsyX/PsyX_render.h"
#include "psx/libgpu.h"

extern "C" {

// ---- PsyX render backend (GR_*) — the rasterizer writes vram[] directly ----
int  GR_InitialisePSX() { return 1; }
int  GR_InitialiseRender(char*, int, int, int) { return 1; }
void GR_Shutdown() {}
void GR_BeginScene() {}
void GR_EndScene() {}
void GR_SwapWindow() {}
void GR_UpdateVRAM() {}
void GR_SetViewPort(int, int, int, int) {}
void GR_SetWireframe(int) {}
void GR_ResetDevice() {}
void GR_StoreFrameBuffer(int, int, int, int) {}
void GR_ReadFramebufferDataToVRAM() {}   // header declares it with no params
void GR_UpdateSwapIntervalState(int) {}

// ---- host window / misc ----
void PsyX_TakeScreenshot() {}

}  // extern "C"

// ---- utils/fs.cpp replacements (POSIX glob is unavailable; the flash VFS
// has no directories, so wildcard enumeration always comes up empty) --------
#include "utils/fs.h"

void FS_FixPathSlashes(char* pathbuff)
{
    for (char* p = pathbuff; p && *p; p++)
        if (*p == '\\') *p = '/';
}

const char* FS_FindFirst(const char* wildcard, FS_FINDDATA** findData)
{
    (void)wildcard;
    if (findData) *findData = NULL;
    return NULL;
}

const char* FS_FindNext(FS_FINDDATA* findData) { (void)findData; return NULL; }
void FS_FindClose(FS_FINDDATA* findData) { (void)findData; }
bool FS_FindIsDirectory(FS_FINDDATA* findData) { (void)findData; return false; }

// ---- remaining GR_* backend entry points (no GPU here; the software
// rasterizer writes vram[] directly, so these are no-ops or trivial) --------
extern "C" {
void GR_Clear(int, int, int, int, unsigned char, unsigned char, unsigned char) {}
void GR_SetBlendMode(BlendMode) {}
void GR_SetStencilMode(int) {}
void GR_SetOffscreenState(const RECT16*, int) {}
void GR_SetupClipMode(const RECT16*, int) {}
void GR_SetTexture(TextureID, TexFormat) {}
void GR_SetOverrideTextureSize(int, int) {}
void GR_UpdateVertexBuffer(const GrVertex*, int) {}
void GR_DrawTriangles(int, int) {}
void GR_PushDebugLabel(const char*) {}
void GR_PopDebugLabel() {}
TextureID GR_CreateRGBATexture(int, int, u_char*) { return 0; }

// VRAM ops are pure memory work — implement them for real
void GR_ClearVRAM(int x, int y, int w, int h, unsigned char r, unsigned char g, unsigned char b)
{
    extern unsigned short vram[VRAM_WIDTH * VRAM_HEIGHT];
    const unsigned short c = (unsigned short)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
    for (int yy = y; yy < y + h && yy < VRAM_HEIGHT; yy++)
        for (int xx = x; xx < x + w && xx < VRAM_WIDTH; xx++)
            vram[yy * VRAM_WIDTH + xx] = c;
}

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

void GR_ReadVRAM(unsigned short* dst, int x, int y, int dst_w, int dst_h)
{
    extern unsigned short vram[VRAM_WIDTH * VRAM_HEIGHT];
    for (int yy = 0; yy < dst_h; yy++)
        for (int xx = 0; xx < dst_w; xx++)
            dst[yy * dst_w + xx] = vram[((y + yy) & (VRAM_HEIGHT - 1)) * VRAM_WIDTH
                                        + ((x + xx) & (VRAM_WIDTH - 1))];
}
}  // extern "C"
