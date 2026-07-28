// Second-core rasterisation.
//
// Both cores walk the SAME ordering table, each restricted to a horizontal band
// of VRAM. Every pixel is therefore written by exactly one core, and because
// each core visits the primitives in ordering-table order, the painter's
// ordering that Driver 2 relies on (there is no depth buffer) is preserved
// within each band.
//
// What makes this safe is that the walk is a pure function of the OT bytes,
// with three exceptions, all handled here or in PsyX_SoftRas.cpp:
//   - the primitive render state (SRState) is thread-local
//   - the draw environment is per-core (g_drawEnv[2]); both cores replay the
//     same DR_TPAGE/DR_TWIN/DR_AREA packets into their own slot and must end
//     bit-identical, which is asserted at the join
//   - primitives with side effects outside the band (VRAM blits and uploads)
//     go through a rendezvous so they happen exactly once
//
// The split line is not fixed: the top half of a Driver 2 frame is sky and the
// bottom half is road, so the pixel load is nowhere near 50/50. It is adjusted
// each frame from the span pixels each core actually drew.
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "PsyX/PsyX_public.h"
#include "PsyX/PsyX_render.h"     // VRAM_HEIGHT
#include "psx/libgpu.h"
#include "gpu/PsyX_GPU.h"

static const char* TAG = "raster";

extern "C" {

extern int g_srBandY0[2], g_srBandY1[2];     // PsyX_SoftRas.cpp
extern unsigned g_srBandPx[2];

static TaskHandle_t sWorker, sMaster;
static u_long* volatile sWalkPtr;
static volatile int sWorkerActive;
static int sSplitY;

int g_srForkEnabled = 1;                     // runtime kill switch
int g_srEnvDiverge;                          // canary, see the join

// Long enough that a real stall is unmistakable, short enough that a
// divergence bug degrades to a glitched frame plus a log line, not a hang.
#define SR_WAIT_TICKS pdMS_TO_TICKS(200)

static void rasterWorker(void*)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        SR_WalkOT(sWalkPtr);
        xTaskNotifyGive(sMaster);
    }
}

void SoftRas_ForkInit(void)
{
    // Internal stack: an ISR runs on the interrupted task's stack on Xtensa,
    // and the panel DMA completion handler is IRAM-resident precisely so it can
    // fire with the flash cache disabled — at which point a PSRAM stack is
    // unreachable.
    // 6 KB: measured high-water use is under 2 KB, and the rest is headroom for
    // an interrupt, which on Xtensa runs on the interrupted task's stack.
    if (xTaskCreatePinnedToCore(rasterWorker, "raster1", 6144, NULL, 6,
                                &sWorker, 1) != pdPASS) {
        ESP_LOGE(TAG, "worker task creation failed - staying single core");
        sWorker = NULL;
        return;
    }
    ESP_LOGI(TAG, "raster worker on core 1");
}

int SoftRas_ForkEnabled(void)
{
    // Not worth forking a band smaller than a couple of DMA chunks.
    return g_srForkEnabled && sWorker && g_drawEnv[0].clip.h >= 32;
}

void SoftRas_ForkOT(u_long* p)
{
    const int y0 = g_drawEnv[0].clip.y;
    const int y1 = y0 + g_drawEnv[0].clip.h - 1;

    if (sSplitY <= y0 + 8 || sSplitY >= y1 - 8)      // first frame or mode change
        sSplitY = y0 + g_drawEnv[0].clip.h / 2;

    // Partition ALL of VRAM, not just the clip rect, so a DR_AREA packet
    // arriving mid-walk cannot move pixels outside the union of the bands.
    g_srBandY0[0] = 0;         g_srBandY1[0] = sSplitY - 1;
    g_srBandY0[1] = sSplitY;   g_srBandY1[1] = VRAM_HEIGHT - 1;

    g_drawEnv[1] = g_drawEnv[0];              // identical seed for the replay
    g_srBandPx[0] = g_srBandPx[1] = 0;

    sWalkPtr = p;
    sMaster = xTaskGetCurrentTaskHandle();
    sWorkerActive = 1;
    xTaskNotifyGive(sWorker);
}

void SoftRas_JoinOT(void)
{
    if (!ulTaskNotifyTake(pdTRUE, SR_WAIT_TICKS))
        ESP_LOGE(TAG, "join timed out - worker stuck");
    sWorkerActive = 0;

    // Canary: the per-core draw-environment replay must land bit-identical.
    // If it ever does not, the two cores saw different primitive streams and
    // the frame is suspect - resync so the error cannot accumulate.
    if (memcmp(&g_drawEnv[0], &g_drawEnv[1], sizeof(DRAWENV)) != 0) {
        g_srEnvDiverge++;
        g_drawEnv[1] = g_drawEnv[0];
    }

    // Move the split toward whichever core drew fewer pixels. Capped at 32
    // lines per frame, and skipped on near-empty frames so it cannot oscillate.
    const int p0 = (int)g_srBandPx[0], p1 = (int)g_srBandPx[1];
    if (p0 + p1 > 4096) {
        sSplitY -= (p0 - p1) * 32 / (p0 + p1);
        const int y0 = g_drawEnv[0].clip.y;
        const int y1 = y0 + g_drawEnv[0].clip.h - 1;
        if (sSplitY < y0 + 8) sSplitY = y0 + 8;
        if (sSplitY > y1 - 8) sSplitY = y1 - 8;
    }
}

// Rendezvous for a primitive whose side effect must happen exactly once.
// Both cores reach every barrier, in the same order, because they walk the same
// list — so the notification counts always pair up.
int SR_BarrierEnter(void)
{
    if (!sWorkerActive)
        return 1;                                   // single core: just do it

    if (xPortGetCoreID() == 1) {
        xTaskNotifyGive(sMaster);                   // "parked"
        if (!ulTaskNotifyTake(pdTRUE, SR_WAIT_TICKS))
            ESP_LOGE(TAG, "barrier resume timed out");
        return 0;
    }

    if (!ulTaskNotifyTake(pdTRUE, SR_WAIT_TICKS))   // wait for the worker to park
        ESP_LOGE(TAG, "barrier park timed out");
    return 1;
}

void SR_BarrierExit(void)
{
    if (sWorkerActive && xPortGetCoreID() == 0)
        xTaskNotifyGive(sWorker);
}

unsigned SoftRas_WorkerStackFree(void)
{
    return sWorker ? uxTaskGetStackHighWaterMark(sWorker) : 0;
}

}  // extern "C"
