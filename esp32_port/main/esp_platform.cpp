// ESP32-S3 platform backend for REDRIVER2 / PsyX.
// Replaces the SDL+OpenGL host layer: the software rasterizer already draws
// into vram[], so present = 555 -> RGB565 straight into the ST7789 DMA
// pipeline shared with the OpenLara/Descent ports.
#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"

#include "PsyX/PsyX_public.h"
#include "PsyX/PsyX_globals.h"
#include "PsyX/PsyX_render.h"
#include "psx/libgpu.h"
#include "psx/libetc.h"

#include "display.h"
#include "board_pins.h"

static const char* TAG = "psx_plat";

extern unsigned short vram[VRAM_WIDTH * VRAM_HEIGHT];
extern "C" {
extern DISPENV activeDispEnv;
extern DRAWENV activeDrawEnv;
extern int g_softRasterEnabled;
}

// PsyX globals the host layer owns
int g_windowWidth = DISPLAY_WIDTH;
int g_windowHeight = DISPLAY_HEIGHT;

// ---- present ---------------------------------------------------------------
// vram is BGR555 (r low). The panel is fed RGB565 by displayFlushRGB565();
// the R/B swap is handled by the ST7789 MADCTL BGR bit, so only green widens.

static uint16_t sLine[DISPLAY_WIDTH * 16];

extern "C" int SoftRas_PresentESP()
{
    const int dx = activeDispEnv.disp.x;
    const int dy = activeDispEnv.disp.y;
    int dw = activeDispEnv.disp.w;
    int dh = activeDispEnv.disp.h;
    if (dw <= 0 || dh <= 0) return 1;
    if (dw > DISPLAY_WIDTH) dw = DISPLAY_WIDTH;
    if (dh > DISPLAY_HEIGHT) dh = DISPLAY_HEIGHT;

    displayFlush555(&vram[(dy & 511) * VRAM_WIDTH + dx], VRAM_WIDTH, dw, dh);
    return 0;
}

// ---- input -----------------------------------------------------------------
// Serial (USB-JTAG) dev keys + the physical pad on header P1, merged into the
// emulated PSX pad button mask that PsyX hands to the game.
//   arrows = d-pad   x = cross   z = circle   q = square   e = triangle
//   Enter = start    space = select   a/d = L1/R1

#define SERIAL_HOLD 6

static uint8_t sHold[16];
static uint16_t sPadMask = 0xFFFF;   // active-low, PSX convention

// PSX pad bit positions (active low in the 16-bit word)
#define PAD_SELECT   0x0001
#define PAD_START    0x0008
#define PAD_UP       0x0010
#define PAD_RIGHT    0x0020
#define PAD_DOWN     0x0040
#define PAD_LEFT     0x0080
#define PAD_L2       0x0100
#define PAD_R2       0x0200
#define PAD_L1       0x0400
#define PAD_R1       0x0800
#define PAD_TRIANGLE 0x1000
#define PAD_CIRCLE   0x2000
#define PAD_CROSS    0x4000
#define PAD_SQUARE   0x8000

static const struct { uint8_t gpio; uint16_t mask; } sButtons[] = {
    { PIN_BTN_UP,     PAD_UP },
    { PIN_BTN_DOWN,   PAD_DOWN },
    { PIN_BTN_LEFT,   PAD_LEFT },
    { PIN_BTN_RIGHT,  PAD_RIGHT },
    { PIN_BTN_A,      PAD_CROSS },
    { PIN_BTN_B,      PAD_SQUARE },
    { PIN_BTN_L,      PAD_L1 },
    { PIN_BTN_R,      PAD_R1 },
    { PIN_BTN_START,  PAD_START },
    { PIN_BTN_SELECT, PAD_SELECT },
};

static int serialBit(uint8_t c)
{
    switch (c) {
        case 'w': return 4;   // UP     (bit index into sHold)
        case 'd': return 5;   // RIGHT
        case 's': return 6;   // DOWN
        case 'a': return 7;   // LEFT
        case 'x': return 14;  // CROSS
        case 'z': return 13;  // CIRCLE
        case 'q': return 15;  // SQUARE
        case 'e': return 12;  // TRIANGLE
        case '\r': case '\n': return 3;  // START
        case ' ': return 0;   // SELECT
        case '1': return 10;  // L1
        case '2': return 11;  // R1
    }
    return -1;
}

extern "C" void esp_input_init()
{
    uint64_t mask = 1ULL << GPIO_NUM_0;
    for (unsigned i = 0; i < sizeof(sButtons) / sizeof(sButtons[0]); i++)
        mask |= 1ULL << sButtons[i].gpio;

    gpio_config_t cfg = {};
    cfg.pin_bit_mask = mask;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&cfg);

    usb_serial_jtag_driver_config_t ucfg = {
        .tx_buffer_size = 1024,
        .rx_buffer_size = 256,
    };
    usb_serial_jtag_driver_install(&ucfg);
}

// called from PsyX's pad read
extern "C" uint16_t esp_input_poll()
{
    uint8_t buf[16];
    int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), 0);
    for (int i = 0; i < n; i++) {
        int b = serialBit(buf[i]);
        if (b >= 0) sHold[b] = SERIAL_HOLD;
    }

    uint16_t mask = 0xFFFF;
    for (int b = 0; b < 16; b++) {
        if (sHold[b]) { sHold[b]--; mask &= ~(1u << b); }
    }

    for (unsigned i = 0; i < sizeof(sButtons) / sizeof(sButtons[0]); i++)
        if (!gpio_get_level((gpio_num_t)sButtons[i].gpio)) mask &= ~sButtons[i].mask;

    if (!gpio_get_level(GPIO_NUM_0)) mask &= ~PAD_CROSS;   // BOOT = X

    sPadMask = mask;
    return mask;
}

// ---- timing ----------------------------------------------------------------

extern "C" uint32_t esp_millis()
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}
