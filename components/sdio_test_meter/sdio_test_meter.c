#include "sdio_test_meter.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "soc/soc.h"

#if CONFIG_IDF_TARGET_ESP32P4
#include "esp_cache.h"
#include "hal/cache_ll.h"
#endif

static const char *TAG = "sdio_test_meter";

// Run-time counter in microseconds (on RISC-V the only run-time stats clock
// is esp_timer) and per-task counters available.
#if configGENERATE_RUN_TIME_STATS && configUSE_TRACE_FACILITY && \
    defined(CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER)
#define HAVE_RUN_TIME 1
#else
#define HAVE_RUN_TIME 0
#endif

#define CORES (configNUMBER_OF_CORES < SDIO_TEST_MAX_CORES ? configNUMBER_OF_CORES : SDIO_TEST_MAX_CORES)
#define INTERNAL_LEN (8 * 1024)
#define PSRAM_LEN (256 * 1024)
#define BUF_ALIGN 128  // P4 L2 cache line; also fine for the L1 (64)
#define PROBE_STACK 3072
#define STOP_WAIT_MS 2000

typedef struct {
    TaskHandle_t task;
    volatile bool stop;
    volatile bool done;
    volatile uint32_t copies;
    uint8_t *buf[2];  // as allocated
    uint8_t *src;     // as accessed (uncached alias on the P4)
    uint8_t *dst;
    size_t len;
    bool uncached;
} probe_t;

static probe_t *s_probe[SDIO_TEST_MAX_CORES];
static uint8_t s_mode = SDIO_TEST_PROBE_OFF;

static void free_bufs(probe_t *p)
{
    for (int i = 0; i < 2; i++) {
        if (p->buf[i] == NULL) {
            continue;
        }
#if CONFIG_IDF_TARGET_ESP32P4
        if (p->uncached) {
            // Written through the uncached alias: drop any line the cache
            // still holds for this range before the heap reuses it.
            esp_cache_msync(p->buf[i], p->len, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
        }
#endif
        heap_caps_free(p->buf[i]);
        p->buf[i] = NULL;
    }
}

static esp_err_t alloc_bufs(probe_t *p, uint8_t mode)
{
    uint32_t caps;
    if (mode == SDIO_TEST_PROBE_INTERNAL) {
        caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        p->len = INTERNAL_LEN;
    } else {
#if CONFIG_SPIRAM
        caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        p->len = PSRAM_LEN;
#else
        return ESP_ERR_NOT_SUPPORTED;
#endif
    }
    for (int i = 0; i < 2; i++) {
        p->buf[i] = heap_caps_aligned_alloc(BUF_ALIGN, p->len, caps);
        if (p->buf[i] == NULL) {
            return ESP_ERR_NO_MEM;
        }
        memset(p->buf[i], 0x5a ^ i, p->len);
    }
    p->src = p->buf[0];
    p->dst = p->buf[1];
#if CONFIG_IDF_TARGET_ESP32P4
    if (mode == SDIO_TEST_PROBE_INTERNAL) {
        for (int i = 0; i < 2; i++) {
            const uintptr_t a = (uintptr_t)p->buf[i];
            if (a < SOC_DRAM_LOW || a + p->len > SOC_DRAM_HIGH) {
                return ESP_ERR_NO_MEM;  // not L2MEM (no uncached alias)
            }
            // Write back and drop the cached copy: from here on only the
            // uncached alias touches these lines.
            if (esp_cache_msync(p->buf[i], p->len,
                                ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE |
                                    ESP_CACHE_MSYNC_FLAG_TYPE_DATA) != ESP_OK) {
                return ESP_ERR_NO_MEM;
            }
        }
        p->uncached = true;
        p->src = (uint8_t *)CACHE_LL_L2MEM_NON_CACHE_ADDR(p->buf[0]);
        p->dst = (uint8_t *)CACHE_LL_L2MEM_NON_CACHE_ADDR(p->buf[1]);
    }
#endif
    return ESP_OK;
}

static void probe_task(void *arg)
{
    probe_t *p = arg;
    while (!p->stop) {
        memcpy(p->dst, p->src, p->len);
        p->copies++;
    }
    free_bufs(p);
    p->done = true;
    // stop_all() deletes us; it still uses the handle until then.
    for (;;) {
        vTaskSuspend(NULL);
    }
}

static void stop_all(void)
{
    for (int c = 0; c < CORES; c++) {
        probe_t *p = s_probe[c];
        if (p == NULL) {
            continue;
        }
        s_probe[c] = NULL;
        p->stop = true;
        // At idle priority it may not run while the core is busy; lift it to
        // ours so it can finish.
        vTaskPrioritySet(p->task, uxTaskPriorityGet(NULL));
        int waited = 0;
        while (!p->done && waited < STOP_WAIT_MS) {
            vTaskDelay(1);
            waited += portTICK_PERIOD_MS;
        }
        if (p->done) {
            vTaskDelete(p->task);
            free(p);
        } else {
            ESP_LOGE(TAG, "probe on core %d did not stop; leaking it", c);
        }
    }
    s_mode = SDIO_TEST_PROBE_OFF;
}

esp_err_t sdio_test_probe_set(uint8_t mode)
{
    if (mode == s_mode) {
        return ESP_OK;
    }
    stop_all();
    if (mode == SDIO_TEST_PROBE_OFF) {
        return ESP_OK;
    }
    if (mode != SDIO_TEST_PROBE_INTERNAL && mode != SDIO_TEST_PROBE_PSRAM) {
        return ESP_ERR_INVALID_ARG;
    }
#if !CONFIG_SPIRAM
    if (mode == SDIO_TEST_PROBE_PSRAM) {
        return ESP_ERR_NOT_SUPPORTED;
    }
#endif
    // Allocate everything first, then start the tasks together.
    esp_err_t err = ESP_OK;
    for (int c = 0; c < CORES && err == ESP_OK; c++) {
        probe_t *p = calloc(1, sizeof(*p));
        if (p == NULL) {
            err = ESP_ERR_NO_MEM;
            break;
        }
        s_probe[c] = p;
        err = alloc_bufs(p, mode);
    }
    for (int c = 0; c < CORES && err == ESP_OK; c++) {
        if (xTaskCreatePinnedToCore(probe_task, "sdio_probe", PROBE_STACK, s_probe[c], tskIDLE_PRIORITY,
                                    &s_probe[c]->task, c) != pdPASS) {
            err = ESP_ERR_NO_MEM;
        }
    }
    if (err != ESP_OK) {
        for (int c = 0; c < CORES; c++) {
            probe_t *p = s_probe[c];
            if (p == NULL) {
                continue;
            }
            if (p->task != NULL) {
                // Started: let it stop and clean up itself.
                continue;
            }
            free_bufs(p);
            free(p);
            s_probe[c] = NULL;
        }
        stop_all();
        ESP_LOGW(TAG, "probe %u: %s", mode, esp_err_to_name(err));
        return err;
    }
    s_mode = mode;
    return ESP_OK;
}

static uint64_t probe_bytes(void)
{
    uint64_t b = 0;
    for (int c = 0; c < CORES; c++) {
        if (s_probe[c] != NULL) {
            b += (uint64_t)s_probe[c]->copies * s_probe[c]->len;
        }
    }
    return b;
}

#if HAVE_RUN_TIME
static configRUN_TIME_COUNTER_TYPE task_run_time(TaskHandle_t t)
{
    TaskStatus_t st;
    vTaskGetInfo(t, &st, pdFALSE, eReady);
    return st.ulRunTimeCounter;
}
#endif

void sdio_test_meter_mark(sdio_test_meter_mark_t *mark)
{
    memset(mark, 0, sizeof(*mark));
    mark->t0_us = esp_timer_get_time();
#if HAVE_RUN_TIME
    for (int c = 0; c < CORES; c++) {
        mark->idle0[c] = ulTaskGetIdleRunTimeCounterForCore(c);
        mark->probe_rt0[c] = s_probe[c] != NULL ? task_run_time(s_probe[c]->task) : 0;
    }
#endif
    mark->probe_bytes0 = probe_bytes();
    sdio_test_axi_mark(&mark->axi);
}

void sdio_test_meter_read(const sdio_test_meter_mark_t *mark, sdio_test_window_t *out)
{
    memset(out, 0, sizeof(*out));
    const int64_t now = esp_timer_get_time();
    const uint32_t window = (uint32_t)(now - mark->t0_us);
    out->window_us = window;
    out->cores = CORES;
    out->probe = s_mode;
    out->probe_ok = s_mode != SDIO_TEST_PROBE_OFF;
    out->probe_bytes = probe_bytes() - mark->probe_bytes0;
    sdio_test_axi_read(&mark->axi, out);
    for (int c = 0; c < SDIO_TEST_MAX_CORES; c++) {
        out->busy_permille[c] = SDIO_TEST_CPU_UNKNOWN;
    }
#if HAVE_RUN_TIME
    uint64_t probe_us = 0;
    for (int c = 0; c < CORES; c++) {
        const configRUN_TIME_COUNTER_TYPE idle =
            ulTaskGetIdleRunTimeCounterForCore(c) - (configRUN_TIME_COUNTER_TYPE)mark->idle0[c];
        configRUN_TIME_COUNTER_TYPE probe = 0;
        if (s_probe[c] != NULL) {
            probe = task_run_time(s_probe[c]->task) - (configRUN_TIME_COUNTER_TYPE)mark->probe_rt0[c];
        }
        probe_us += probe;
        const uint64_t free_us = (uint64_t)idle + probe;
        const uint64_t busy = window > free_us ? window - free_us : 0;
        out->busy_permille[c] = window ? (uint16_t)(busy * 1000 / window) : 0;
    }
    out->probe_us = (uint32_t)probe_us;
#endif
}
