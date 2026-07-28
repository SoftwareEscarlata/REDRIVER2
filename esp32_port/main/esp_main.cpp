// Driver 2 (REDRIVER2) on the ESP32-S3 — entry point.
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"

#include "esp_fs.h"
#include "display.h"

static const char* TAG = "driver2";

extern int redriver2_main(int argc, char** argv);   // Game/C/main.c

extern "C" void esp_input_init();
extern "C" int g_softRasterEnabled;

static void gameTask(void*)
{
    ESP_LOGI(TAG, "internal free: %u KB (largest %u KB), PSRAM free: %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >> 10),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >> 10));

    static char arg0[] = "redriver2";
    static char* argv[] = { arg0, nullptr };
    int r = redriver2_main(1, argv);
    ESP_LOGI(TAG, "redriver2_main returned %d", r);
    vTaskDelete(NULL);
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Driver 2 ESP32-S3 (REDRIVER2 + software rasterizer)");
    ESP_LOGI(TAG, "PSRAM: %u KB", (unsigned)(esp_psram_get_size() >> 10));

    g_softRasterEnabled = 1;   // no OpenGL here — the rasterizer owns the frame

    if (esp_fs_mount() != 0) {
        ESP_LOGE(TAG, "game data mount failed — flash gamedata.bin first");
        return;
    }

    if (displayInit() != 0) {
        ESP_LOGE(TAG, "display init failed");
        return;
    }

    esp_input_init();

    // big stack in PSRAM (the PSX code recurses through the renderer);
    // static creation is required for an external stack
    static StaticTask_t tcb;
    const uint32_t stackBytes = 96 * 1024;
    StackType_t* stack = (StackType_t*)heap_caps_malloc(stackBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!stack) {
        ESP_LOGE(TAG, "no PSRAM for game stack");
        return;
    }
    xTaskCreateStaticPinnedToCore(gameTask, "game", stackBytes, NULL, 5, stack, &tcb, 0);
}
