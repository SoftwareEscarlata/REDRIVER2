// Minimal SDL shim for the ESP32 build.
// Game code includes <SDL.h> for a handful of things — message boxes, a
// delay, and (in dead debug paths) surface/window types. Nothing here talks
// to a real SDL; message boxes go to the serial log.
#pragma once

#include <stdint.h>
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SDL_MESSAGEBOX_ERROR       0x10
#define SDL_MESSAGEBOX_WARNING     0x20
#define SDL_MESSAGEBOX_INFORMATION 0x40
#define SDL_WINDOWPOS_UNDEFINED    0
#define SDL_WINDOW_TOOLTIP         0

typedef struct SDL_Window SDL_Window;
typedef struct SDL_Texture SDL_Texture;
typedef struct SDL_Renderer SDL_Renderer;

typedef struct { int x, y, w, h; } SDL_Rect;

typedef struct {
    int w, h, pitch;
    void* pixels;
} SDL_Surface;

static inline int SDL_ShowSimpleMessageBox(uint32_t flags, const char* title,
                                           const char* message, SDL_Window* window)
{
    (void)flags; (void)window;
    ESP_LOGE("game", "%s: %s", title ? title : "message", message ? message : "");
    return 0;
}

static inline void SDL_Delay(uint32_t ms)
{
    extern void vTaskDelay(uint32_t);   // ticks == ms at CONFIG_FREERTOS_HZ=1000
    vTaskDelay(ms ? ms : 1);
}

static inline const char* SDL_GetError(void) { return ""; }

// dead debug paths only — never called on this target
static inline SDL_Surface* SDL_GetWindowSurface(SDL_Window* w) { (void)w; return 0; }
static inline int  SDL_LockSurface(SDL_Surface* s) { (void)s; return -1; }
static inline void SDL_UnlockSurface(SDL_Surface* s) { (void)s; }
static inline int  SDL_UpdateWindowSurface(SDL_Window* w) { (void)w; return -1; }

#ifdef __cplusplus
}
#endif
