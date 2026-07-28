// Stubs for host subsystems not present on the ESP32 build:
// PsyX render/audio backends, SDL window/input plumbing, memory cards.
// The software rasterizer + esp_platform.cpp cover what the game actually needs.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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
