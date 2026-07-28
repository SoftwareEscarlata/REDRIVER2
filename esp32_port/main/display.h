// Display layer for the OpenLara ESP32-S3 port.
// Three functions hide the backend (esp_lcd native / LovyanGFX) so swapping
// implementations is a build-system change, not a code change.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DISPLAY_WIDTH   320
#define DISPLAY_HEIGHT  240

// Init bus + panel + backlight. Returns 0 on success.
int displayInit(void);

// 256-entry palette in OpenLara's BGR555 format (bits 0-4 = R, 5-9 = G, 10-14 = B).
// Converted internally to pre-byteswapped RGB565 for the panel.
void displaySetPalette(const uint16_t* pal256);

// Push a full 8bpp indexed frame (DISPLAY_WIDTH*DISPLAY_HEIGHT bytes).
// Expands through the palette into DMA line buffers, chunked, overlapping
// expansion with the previous chunk's SPI transfer.
void displayFlush(const uint8_t* fb8);

// Push a BGR555 source rect (PSX VRAM format: r | g<<5 | b<<10) as RGB565.
// `stride` is the source pitch in uint16 units. The panel is configured with
// the MADCTL BGR bit so only the green channel needs widening 5->6.
void displayFlush555(const uint16_t* src, int stride, int w, int h);

// Same, but rescales an arbitrary sw*sh source to fill the whole panel
// (nearest-neighbour). Needed because the PSX changes video mode at runtime.
void displayFlush555Scaled(const uint16_t* src, int stride, int sw, int sh);

// Start the present worker on the second core. Returns 0 on success; without it
// displayPresentAsync() silently falls back to the blocking path.
int displayPresentAsyncInit(void);

// Hand a flush to the worker and return immediately. The source must stay
// untouched until the next displayPresentAsync/displayPresentWait returns.
void displayPresentAsync(const uint16_t* src, int stride, int sw, int sh);

// Block until any in-flight asynchronous flush has completed.
void displayPresentWait(void);

// Blocks until the last displayFlush transfer fully completed.
void displayWaitFlush(void);

#ifdef __cplusplus
}
#endif
