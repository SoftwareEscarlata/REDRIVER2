// esp_lcd backend for the OpenLara display layer.
// ST7789 over SPI, chunked DMA streaming with palette expansion overlapped
// against the in-flight transfer:
//
//   fb8 (8bpp, internal SRAM)
//     -> expand N lines through LUT into ping-pong DMA buffer A/B
//     -> esp_lcd_panel_draw_bitmap (async DMA)  while expanding the next chunk
//
// Pin assignments live in board_pins.h.
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "display.h"
#include "board_pins.h"

static const char* TAG = "display";

// lines per DMA chunk: 48 lines * 320 px * 2 B = 30720 B — just under the S3's
// 32768-byte-per-transaction hardware cap, so each band is exactly ONE SPI
// transaction (5 per frame, ~0.2 ms total overhead). x2 ping-pong = 60 KB DMA RAM.
// If SRAM gets tight in later phases, 16-line bands cost only ~0.4 ms more.
// [descent] 16-line bands (10240B x2 = 20KB DMA RAM instead of 60KB) — internal
// SRAM is tighter here than in OpenLara; costs ~0.4ms/frame extra overhead
#define CHUNK_LINES 16
#define CHUNK_BYTES (DISPLAY_WIDTH * CHUNK_LINES * 2)

static esp_lcd_panel_handle_t sPanel;
static esp_lcd_panel_io_handle_t sIO;
static SemaphoreHandle_t sTransDone;

static uint16_t sLut[256];        // pre-byteswapped RGB565
static uint16_t* sChunk[2];       // ping-pong DMA buffers (internal, DMA-capable)

static bool IRAM_ATTR onTransDone(esp_lcd_panel_io_handle_t io,
                                  esp_lcd_panel_io_event_data_t* ev, void* user)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(sTransDone, &woken);
    return woken == pdTRUE;
}

int displayInit(void)
{
    // backlight off during init (boots dark; we turn it on after the first
    // black frame so panel init garbage is never visible)
    gpio_config_t bl = {
        .pin_bit_mask = (1ULL << PIN_LCD_BL) | (1ULL << PIN_SD_CS),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bl);
    gpio_set_level(PIN_LCD_BL, !LCD_BL_ON_LEVEL);
    // shared SPI2 bus: keep the microSD deselected so it never drives MISO
    gpio_set_level(PIN_SD_CS, 1);

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_SD_MISO,   // for the SD card; LCD IO stays write-only
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = CHUNK_BYTES,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_SPI_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 8,
        .on_color_trans_done = onTransDone,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
                                             &io_cfg, &sIO));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ORDER,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(sIO, &panel_cfg, &sPanel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(sPanel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(sPanel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(sPanel, LCD_INVERT_COLOR));
    // native 240x320 portrait -> 320x240 landscape
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(sPanel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(sPanel, LCD_MIRROR_X, LCD_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(sPanel, LCD_GAP_X, LCD_GAP_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(sPanel, true));

    // counts FREE ping-pong buffers: take one before writing into a buffer,
    // each on_color_trans_done returns one. Guarantees we never overwrite a
    // buffer whose DMA transfer is still in flight.
    sTransDone = xSemaphoreCreateCounting(2, 2);

    for (int i = 0; i < 2; i++) {
        sChunk[i] = heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (!sChunk[i]) {
            ESP_LOGE(TAG, "no DMA memory for chunk buffers");
            return -1;
        }
    }

    // push one black frame before enabling the backlight
    memset(sChunk[0], 0, CHUNK_BYTES);
    for (int y = 0; y < DISPLAY_HEIGHT; y += CHUNK_LINES) {
        xSemaphoreTake(sTransDone, portMAX_DELAY);
        esp_lcd_panel_draw_bitmap(sPanel, 0, y, DISPLAY_WIDTH, y + CHUNK_LINES, sChunk[0]);
    }
    displayWaitFlush();
    gpio_set_level(PIN_LCD_BL, LCD_BL_ON_LEVEL);

    ESP_LOGI(TAG, "ST7789 up: %d Hz SPI, %d-line chunks", LCD_SPI_HZ, CHUNK_LINES);
    return 0;
}

void displaySetPalette(const uint16_t* pal256)
{
    // OpenLara BGR555 (R low) -> RGB565, byteswapped for the panel
    for (int i = 0; i < 256; i++) {
        uint16_t c = pal256[i];
        uint16_t r = (c & 0x1F);
        uint16_t g = (c >> 5) & 0x1F;
        uint16_t b = (c >> 10) & 0x1F;
        uint16_t rgb565 = (r << 11) | (g << 6) | b;   // 5-6-5, G gets the extra bit
        sLut[i] = (rgb565 >> 8) | (rgb565 << 8);      // SPI sends MSB first
    }
}

static inline void expandChunk(const uint8_t* restrict src, uint16_t* restrict dst, int pixels)
{
    // 2 pixels per 32-bit store, ~4-5 cycles/pixel on the LX7; the 512B LUT
    // stays cache-resident. A 48-line band (~0.3 ms) hides entirely inside
    // the previous band's ~3 ms SPI transfer.
    uint32_t* restrict d = (uint32_t*)dst;
    for (int i = 0; i < pixels; i += 2) {
        *d++ = (uint32_t)sLut[src[i]] | ((uint32_t)sLut[src[i + 1]] << 16);
    }
}

void displayFlush(const uint8_t* fb8)
{
    int buf = 0;
    for (int y = 0; y < DISPLAY_HEIGHT; y += CHUNK_LINES) {
        int lines = (y + CHUNK_LINES <= DISPLAY_HEIGHT) ? CHUNK_LINES : (DISPLAY_HEIGHT - y);

        // acquire a free buffer BEFORE writing into it
        xSemaphoreTake(sTransDone, portMAX_DELAY);
        expandChunk(fb8 + y * DISPLAY_WIDTH, sChunk[buf], lines * DISPLAY_WIDTH);
        esp_lcd_panel_draw_bitmap(sPanel, 0, y, DISPLAY_WIDTH, y + lines, sChunk[buf]);

        buf ^= 1;
    }
}

void displayWaitFlush(void)
{
    // drain: both buffers free == no transfer in flight
    for (int i = 0; i < 2; i++) xSemaphoreTake(sTransDone, portMAX_DELAY);
    for (int i = 0; i < 2; i++) xSemaphoreGive(sTransDone);
}

// ---------------------------------------------------------------------------
// BGR555 (PSX VRAM) -> RGB565 band flush, for the REDRIVER2 software rasterizer
// ---------------------------------------------------------------------------
void displayFlush555(const uint16_t* src, int stride, int w, int h)
{
    if (w > DISPLAY_WIDTH) w = DISPLAY_WIDTH;
    if (h > DISPLAY_HEIGHT) h = DISPLAY_HEIGHT;

    int buf = 0;
    for (int y = 0; y < h; y += CHUNK_LINES) {
        int lines = (y + CHUNK_LINES <= h) ? CHUNK_LINES : (h - y);

        xSemaphoreTake(sTransDone, portMAX_DELAY);
        uint16_t* dst = sChunk[buf];
        for (int l = 0; l < lines; l++) {
            const uint16_t* s = src + (y + l) * stride;
            for (int x = 0; x < w; x++) {
                uint16_t c = s[x];
                // 555 -> 565: keep R/B in place (MADCTL BGR handles the swap),
                // widen green by replicating its top bit
                uint16_t g5 = (c >> 5) & 0x1F;
                uint16_t v = (uint16_t)((c & 0x1F) | (((g5 << 1) | (g5 >> 4)) << 5) | (((c >> 10) & 0x1F) << 11));
                dst[l * w + x] = (uint16_t)((v >> 8) | (v << 8));   // panel wants MSB first
            }
        }
        esp_lcd_panel_draw_bitmap(sPanel, 0, y, w, y + lines, dst);
        buf ^= 1;
    }
}

// ---------------------------------------------------------------------------
// Asynchronous present
//
// A full 320x240 16bpp frame is 153,600 bytes; at the panel's 80 MHz SPI clock
// that is 15.4 ms of pure transfer, and the measured present cost was 17 ms.
// So the present is transfer-bound, not CPU-bound: nearly all of it was the
// game task sitting on the DMA-complete semaphore. Handing the flush to a task
// on the other core takes that whole cost off the frame's critical path.
//
// Safe because the PSX double-buffers: the frame being transmitted lives in a
// different half of VRAM from the one being drawn. The caller is expected to
// check that (see SoftRas_PresentESP) and fall back to the blocking path when
// the two overlap, as they do in the 640x512 frontend.
// ---------------------------------------------------------------------------

static TaskHandle_t sPresentTask;
static SemaphoreHandle_t sPresentIdle;      // given when a flush has completed

static struct {
    const uint16_t* src;
    int stride, sw, sh;
} sPresentJob;

static void presentTask(void* arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        displayFlush555Scaled(sPresentJob.src, sPresentJob.stride,
                              sPresentJob.sw, sPresentJob.sh);
        xSemaphoreGive(sPresentIdle);
    }
}

int displayPresentAsyncInit(void)
{
    sPresentIdle = xSemaphoreCreateBinary();
    if (!sPresentIdle) return -1;
    xSemaphoreGive(sPresentIdle);           // starts idle

    // pinned to core 1: core 0 runs the game task
    if (xTaskCreatePinnedToCore(presentTask, "present", 3072, NULL, 4,
                                &sPresentTask, 1) != pdPASS) {
        ESP_LOGE(TAG, "present task creation failed");
        return -1;
    }
    return 0;
}

void displayPresentWait(void)
{
    if (!sPresentIdle) return;
    xSemaphoreTake(sPresentIdle, portMAX_DELAY);
    xSemaphoreGive(sPresentIdle);
}

void displayPresentAsync(const uint16_t* src, int stride, int sw, int sh)
{
    if (!sPresentTask) {                    // no worker: just do it inline
        displayFlush555Scaled(src, stride, sw, sh);
        return;
    }
    xSemaphoreTake(sPresentIdle, portMAX_DELAY);   // previous flush finished
    sPresentJob.src = src;
    sPresentJob.stride = stride;
    sPresentJob.sw = sw;
    sPresentJob.sh = sh;
    xTaskNotifyGive(sPresentTask);
}

void displayFlush555Scaled(const uint16_t* src, int stride, int sw, int sh)
{
    if (sw <= 0 || sh <= 0) return;

    // 16.16 nearest-neighbour steps, so any PSX video mode fills the panel.
    // Driver 2 needs this: the frontend runs 640x512 hi-res while the game
    // itself runs 320x240. At 320x240 the steps are exactly 1.0 (no resampling).
    const uint32_t xStep = ((uint32_t)sw << 16) / DISPLAY_WIDTH;
    const uint32_t yStep = ((uint32_t)sh << 16) / DISPLAY_HEIGHT;

    int buf = 0;
    for (int y = 0; y < DISPLAY_HEIGHT; y += CHUNK_LINES) {
        int lines = (y + CHUNK_LINES <= DISPLAY_HEIGHT) ? CHUNK_LINES : (DISPLAY_HEIGHT - y);

        xSemaphoreTake(sTransDone, portMAX_DELAY);
        uint16_t* dst = sChunk[buf];
        for (int l = 0; l < lines; l++) {
            const uint16_t* s = src + (((uint32_t)(y + l) * yStep) >> 16) * stride;
            uint16_t* d = dst + l * DISPLAY_WIDTH;
            uint32_t sx = 0;
            for (int x = 0; x < DISPLAY_WIDTH; x++, sx += xStep) {
                uint16_t c = s[sx >> 16];
                uint16_t g5 = (c >> 5) & 0x1F;
                uint16_t v = (uint16_t)((c & 0x1F) | (((g5 << 1) | (g5 >> 4)) << 5) | (((c >> 10) & 0x1F) << 11));
                d[x] = (uint16_t)((v >> 8) | (v << 8));
            }
        }
        esp_lcd_panel_draw_bitmap(sPanel, 0, y, DISPLAY_WIDTH, y + lines, dst);
        buf ^= 1;
    }
}
