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
#ifdef SOFTRAS_PROFILE
#include "esp_cpu.h"
#endif
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
void SoftRas_DebugDecodeTPage(int baseX, int baseY, int clutX, int clutY,
                              int u0, int v0, uint16_t* out);
}

// PsyX globals the host layer owns
int g_windowWidth = DISPLAY_WIDTH;
int g_windowHeight = DISPLAY_HEIGHT;

// ---- present ---------------------------------------------------------------
// vram is BGR555 (r low). The panel is fed RGB565 by displayFlushRGB565();
// the R/B swap is handled by the ST7789 MADCTL BGR bit, so only green widens.

static uint16_t sLine[DISPLAY_WIDTH * 16];

// [dbg] number of primitives the rasterizer has consumed since boot
extern "C" { int g_dbgSoftRasPrims = 0; }

#ifdef SOFTRAS_PROFILE
// C++ linkage: matches the `extern unsigned` declarations in the PsyCross files
unsigned g_dbgRasCycles = 0, g_dbgRasBboxPx = 0, g_dbgRasTris = 0, g_dbgPresentCycles = 0;
#endif

// [dbg] 'p' over serial dumps the display area as 160x120 RGB332 hex, so the
// on-panel image can be reconstructed on the host without a camera.
static volatile int sDumpRequest = 0;

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

extern "C" int SoftRas_PresentESP()
{
    // The PSX switches video mode at runtime: Driver 2's frontend draws
    // 640x512 (hi-res interlaced) and the game itself 320x240. Rescale
    // whatever the display env asks for onto the fixed 320x240 panel instead
    // of cropping it.
    const int dx = activeDispEnv.disp.x & (VRAM_WIDTH - 1);
    const int dy = activeDispEnv.disp.y & (VRAM_HEIGHT - 1);
    int dw = activeDispEnv.disp.w;
    int dh = activeDispEnv.disp.h;
    if (dw <= 0 || dh <= 0) return 1;
    if (dw > VRAM_WIDTH - dx)  dw = VRAM_WIDTH - dx;    // never read past VRAM
    if (dh > VRAM_HEIGHT - dy) dh = VRAM_HEIGHT - dy;

    if (sDumpRequest == 2) {                 // 'v': whole VRAM, half scale
        sDumpRequest = 0;
        dumpScreen(vram, VRAM_WIDTH, VRAM_WIDTH, VRAM_HEIGHT, 512, 256);
    } else if (sDumpRequest == 4) {          // 'r': raw VRAM words of one tpage
        sDumpRequest = 0;
        printf("\n#RAW 448 0 64 256\n");
        for (int y = 0; y < 256; y++) {
            char line[257];
            static const char hex[] = "0123456789abcdef";
            for (int x = 0; x < 64; x++) {
                const uint16_t w = vram[y * VRAM_WIDTH + 448 + x];
                line[x * 4 + 0] = hex[(w >> 12) & 15];
                line[x * 4 + 1] = hex[(w >> 8) & 15];
                line[x * 4 + 2] = hex[(w >> 4) & 15];
                line[x * 4 + 3] = hex[w & 15];
            }
            line[256] = 0;
            printf("%s\n", line);
            vTaskDelay(1);
        }
        printf("#ENDRAW\n");
    } else if (sDumpRequest == 3) {          // 't': road texture page, decoded
        sDumpRequest = 0;
        static uint16_t page[128 * 128];
        // in-game scenery page, 1:1 over the region the tree quads sample
        SoftRas_DebugDecodeTPage(704, 0, 960, 294, 96, 72, page);
        dumpScreen(page, 128, 128, 128, 128, 128);
    } else if (sDumpRequest) {               // 'p': what is on the panel
        sDumpRequest = 0;
        dumpScreen(&vram[dy * VRAM_WIDTH + dx], VRAM_WIDTH, dw, dh, 160, 120);
    }

    // We are called at end of frame, so `dy` is the buffer that has just been
    // finished. Handing its flush to the other core is safe exactly when the
    // game is double-buffering, because then the next frame is drawn into the
    // other half of VRAM and cannot touch the bytes being transmitted.
    //
    // Detect that from the display origin alternating between frames: in game
    // it toggles 0 / 256 every frame, while the 640x512 frontend keeps both
    // buffers at the same place and must therefore flush inline.
    static int lastDy = -1;
    const bool doubleBuffered = (lastDy >= 0 && lastDy != dy);
    lastDy = dy;

#ifdef SOFTRAS_PROFILE
    const unsigned c0 = esp_cpu_get_cycle_count();
#endif
    if (doubleBuffered)
        displayPresentAsync(&vram[dy * VRAM_WIDTH + dx], VRAM_WIDTH, dw, dh);
    else
        displayFlush555Scaled(&vram[dy * VRAM_WIDTH + dx], VRAM_WIDTH, dw, dh);
#ifdef SOFTRAS_PROFILE
    g_dbgPresentCycles += esp_cpu_get_cycle_count() - c0;
#endif
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
static uint16_t sButtonStuck = 0;    // button lines this board holds low itself

// PSX pad bits, active low, in the word the game builds as
// (padRaw->buttons[0] << 8) | padRaw->buttons[1]  (see MapPad in pad.c).
// buttons[0] carries d-pad/start/select, buttons[1] the face and shoulder keys.
#define PAD_L2       0x0001
#define PAD_R2       0x0002
#define PAD_L1       0x0004
#define PAD_R1       0x0008
#define PAD_TRIANGLE 0x0010
#define PAD_CIRCLE   0x0020
#define PAD_CROSS    0x0040
#define PAD_SQUARE   0x0080
#define PAD_SELECT   0x0100
#define PAD_START    0x0800
#define PAD_UP       0x1000
#define PAD_RIGHT    0x2000
#define PAD_DOWN     0x4000
#define PAD_LEFT     0x8000

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

// Dev keys over serial -> the same masks the physical buttons produce.
static uint16_t serialMask(uint8_t c)
{
    switch (c) {
        case 'w': return PAD_UP;
        case 'd': return PAD_RIGHT;
        case 's': return PAD_DOWN;
        case 'a': return PAD_LEFT;
        case 'x': return PAD_CROSS;
        case 'z': return PAD_CIRCLE;
        case 'q': return PAD_SQUARE;
        case 'e': return PAD_TRIANGLE;
        case '\r': case '\n': return PAD_START;
        case ' ': return PAD_SELECT;
        case '1': return PAD_L1;
        case '2': return PAD_R1;
    }
    return 0;
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
        if (buf[i] == 'p') { sDumpRequest = 1; continue; }
        if (buf[i] == 'v') { sDumpRequest = 2; continue; }
        if (buf[i] == 't') { sDumpRequest = 3; continue; }
        if (buf[i] == 'r') { sDumpRequest = 4; continue; }
        const uint16_t m = serialMask(buf[i]);
        if (m) sHold[__builtin_ctz(m)] = SERIAL_HOLD;   // held for a few frames
    }

    uint16_t mask = 0xFFFF;
    for (int b = 0; b < 16; b++) {
        if (sHold[b]) { sHold[b]--; mask &= ~(1u << b); }
    }

    for (unsigned i = 0; i < sizeof(sButtons) / sizeof(sButtons[0]); i++)
        if (!(sButtonStuck & (1u << i)) && !gpio_get_level((gpio_num_t)sButtons[i].gpio))
            mask &= ~sButtons[i].mask;

    if (!gpio_get_level(GPIO_NUM_0)) mask &= ~PAD_CROSS;   // BOOT = X

    sPadMask = mask;
    return mask;
}

// ---- timing ----------------------------------------------------------------

extern "C" uint32_t esp_millis()
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}
