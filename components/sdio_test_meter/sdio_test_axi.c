// AXI performance monitor (ESP32-P4 chip revision 3.0 and later; TRM v1.0
// chapter 24). IDF has no driver for it: registers are written directly.
//
// Each of the two monitor units watches one AXI port and runs an
// instantaneous-bandwidth measurement: the hardware counts read and write data
// beats in back-to-back windows of WINDOW_CYCLES bus cycles. Per window of the
// test this gives:
//   max   the busiest monitor window: metric counters 0 (read data) and 2
//         (write data), whose result register holds the min [15:0] and max
//         [31:16] window count since the metric was (re)selected
//   mean  the four most recent window counts (INS_BANDW_DATA0..3, read [15:0],
//         write [31:16]) sampled every SAMPLE_US by an esp_timer
// Both are beats per bus cycle, so they read as the share of the port's data
// channel in use, independent of the bus clock and data width.

#include "sdio_test_axi.h"

#include <string.h>

#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32P4

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "hal/efuse_hal.h"
#include "soc/reg_base.h"
#include "soc/soc.h"

static const char *TAG = "sdio_test_axi";

#define AXI_BASE 0x500A8000u  // AXI_PERF_MON (esp32p4.peripherals.ld)
#define AXI_REG(off) (AXI_BASE + (off))
#define OFF_CLK_EN 0x000
#define OFF_AGENT_SELECT 0x004  // unit 0 [4:0], unit 1 [9:5] (TRM 24.5.4)
#define OFF_COUNTER(unit, m) (0x008 + 8 * (m) + 4 * (unit))
#define OFF_INS_DATA(unit, m) (0x0a8 + 8 * (m) + 4 * (unit))
#define OFF_METRIC_SELECT1(unit) (0x158 + 4 * (unit))
#define OFF_INS_TEST_EN 0x17c  // unit 0 rd [0] wr [1], unit 1 rd [2] wr [3]
#define OFF_WR_STROBE(unit) (0x1a8 + 4 * (unit))
#define OFF_RD_STROBE(unit) (0x1b0 + 4 * (unit))
#define OFF_INS_TIME_THR(unit) (0x1b8 + 4 * (unit))  // rd [15:0], wr [31:16]
#define CLKRST_CTRL0 (DR_REG_HP_SYS_CLKRST_BASE + 0xf0)  // clk_en [0], rst_en [1]

// Metric counters 0..3: read data, read time, write data, write time of the
// instantaneous bandwidth measurement (TRM table 24.5-1: 14..17).
#define METRICS (14u | (15u << 8) | (16u << 16) | (17u << 24))
#define WINDOW_CYCLES 50000u
#define SAMPLE_US 1000
#define RECORDS 4

static bool s_clock_on;
static uint8_t s_agent[SDIO_TEST_AXI_UNITS];
static esp_timer_handle_t s_timer;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint64_t s_rd_sum[SDIO_TEST_AXI_UNITS];
static uint64_t s_wr_sum[SDIO_TEST_AXI_UNITS];
static uint32_t s_samples;

bool sdio_test_axi_available(void)
{
    return efuse_hal_chip_revision() >= 300;
}

static void sample(void *arg)
{
    uint32_t rd[SDIO_TEST_AXI_UNITS] = {0};
    uint32_t wr[SDIO_TEST_AXI_UNITS] = {0};
    for (int u = 0; u < SDIO_TEST_AXI_UNITS; u++) {
        if (s_agent[u] == SDIO_TEST_AXI_NONE) {
            continue;
        }
        for (int m = 0; m < RECORDS; m++) {
            const uint32_t v = REG_READ(AXI_REG(OFF_INS_DATA(u, m)));
            rd[u] += v & 0xffff;
            wr[u] += v >> 16;
        }
    }
    portENTER_CRITICAL(&s_mux);
    for (int u = 0; u < SDIO_TEST_AXI_UNITS; u++) {
        s_rd_sum[u] += rd[u];
        s_wr_sum[u] += wr[u];
    }
    s_samples++;
    portEXIT_CRITICAL(&s_mux);
}

static void monitor_off(void)
{
    if (s_timer != NULL) {
        esp_timer_stop(s_timer);
    }
    REG_WRITE(AXI_REG(OFF_INS_TEST_EN), 0);
    for (int u = 0; u < SDIO_TEST_AXI_UNITS; u++) {
        REG_WRITE(AXI_REG(OFF_METRIC_SELECT1(u)), 0);
        s_agent[u] = SDIO_TEST_AXI_NONE;
    }
}

esp_err_t sdio_test_axi_set(const uint8_t agent[SDIO_TEST_AXI_UNITS])
{
    bool any = false;
    for (int u = 0; u < SDIO_TEST_AXI_UNITS; u++) {
        if (agent[u] > SDIO_TEST_AXI_LCD) {
            return ESP_ERR_INVALID_ARG;
        }
        any |= agent[u] != SDIO_TEST_AXI_NONE;
    }
    if (!sdio_test_axi_available()) {
        return any ? ESP_ERR_NOT_SUPPORTED : ESP_OK;
    }
    if (!s_clock_on) {
        if (!any) {
            return ESP_OK;
        }
        REG_SET_BIT(CLKRST_CTRL0, BIT(0));
        REG_SET_BIT(CLKRST_CTRL0, BIT(1));
        REG_CLR_BIT(CLKRST_CTRL0, BIT(1));
        REG_SET_BIT(AXI_REG(OFF_CLK_EN), BIT(0));
        s_clock_on = true;
    }
    monitor_off();
    if (!any) {
        return ESP_OK;
    }
    if (s_timer == NULL) {
        const esp_timer_create_args_t t = {.callback = sample, .name = "sdio_axi"};
        if (esp_timer_create(&t, &s_timer) != ESP_OK) {
            return ESP_ERR_NO_MEM;
        }
    }
    REG_WRITE(AXI_REG(OFF_AGENT_SELECT), (uint32_t)agent[0] | ((uint32_t)agent[1] << 5));
    uint32_t en = 0;
    for (int u = 0; u < SDIO_TEST_AXI_UNITS; u++) {
        if (agent[u] == SDIO_TEST_AXI_NONE) {
            continue;
        }
        REG_WRITE(AXI_REG(OFF_RD_STROBE(u)), 0);  // count every cycle and beat
        REG_WRITE(AXI_REG(OFF_WR_STROBE(u)), 0);
        REG_WRITE(AXI_REG(OFF_INS_TIME_THR(u)), WINDOW_CYCLES | (WINDOW_CYCLES << 16));
        REG_WRITE(AXI_REG(OFF_METRIC_SELECT1(u)), METRICS);
        en |= 3u << (2 * u);
        s_agent[u] = agent[u];
    }
    REG_WRITE(AXI_REG(OFF_INS_TEST_EN), en);
    if (esp_timer_start_periodic(s_timer, SAMPLE_US) != ESP_OK) {
        monitor_off();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "AXI monitor on ports %u, %u", agent[0], agent[1]);
    return ESP_OK;
}

void sdio_test_axi_mark(sdio_test_axi_mark_t *mark)
{
    for (int u = 0; u < SDIO_TEST_AXI_UNITS; u++) {
        if (s_agent[u] != SDIO_TEST_AXI_NONE) {
            // Reselecting the metrics clears the min / max results.
            REG_WRITE(AXI_REG(OFF_METRIC_SELECT1(u)), 0);
            REG_WRITE(AXI_REG(OFF_METRIC_SELECT1(u)), METRICS);
        }
    }
    portENTER_CRITICAL(&s_mux);
    memcpy(mark->rd_sum, s_rd_sum, sizeof(mark->rd_sum));
    memcpy(mark->wr_sum, s_wr_sum, sizeof(mark->wr_sum));
    mark->samples = s_samples;
    portEXIT_CRITICAL(&s_mux);
}

static uint16_t permille(uint64_t beats, uint64_t cycles)
{
    if (cycles == 0) {
        return 0;
    }
    const uint64_t v = beats * 1000 / cycles;
    return v > 0xfffe ? 0xfffe : (uint16_t)v;
}

void sdio_test_axi_read(const sdio_test_axi_mark_t *mark, sdio_test_window_t *out)
{
    uint64_t rd[SDIO_TEST_AXI_UNITS], wr[SDIO_TEST_AXI_UNITS];
    portENTER_CRITICAL(&s_mux);
    memcpy(rd, s_rd_sum, sizeof(rd));
    memcpy(wr, s_wr_sum, sizeof(wr));
    const uint32_t samples = s_samples - mark->samples;
    portEXIT_CRITICAL(&s_mux);
    out->axi_samples = samples;
    const uint64_t cycles = (uint64_t)samples * RECORDS * WINDOW_CYCLES;
    for (int u = 0; u < SDIO_TEST_AXI_UNITS; u++) {
        out->axi_agent[u] = s_agent[u];
        if (s_agent[u] == SDIO_TEST_AXI_NONE) {
            continue;
        }
        out->axi_rd_mean_permille[u] = permille(rd[u] - mark->rd_sum[u], cycles);
        out->axi_wr_mean_permille[u] = permille(wr[u] - mark->wr_sum[u], cycles);
        out->axi_rd_max_permille[u] = permille(REG_READ(AXI_REG(OFF_COUNTER(u, 0))) >> 16, WINDOW_CYCLES);
        out->axi_wr_max_permille[u] = permille(REG_READ(AXI_REG(OFF_COUNTER(u, 2))) >> 16, WINDOW_CYCLES);
    }
}

#else  // no AXI performance monitor on this chip

bool sdio_test_axi_available(void)
{
    return false;
}

esp_err_t sdio_test_axi_set(const uint8_t agent[SDIO_TEST_AXI_UNITS])
{
    for (int u = 0; u < SDIO_TEST_AXI_UNITS; u++) {
        if (agent[u] != SDIO_TEST_AXI_NONE) {
            return ESP_ERR_NOT_SUPPORTED;
        }
    }
    return ESP_OK;
}

void sdio_test_axi_mark(sdio_test_axi_mark_t *mark)
{
    memset(mark, 0, sizeof(*mark));
}

void sdio_test_axi_read(const sdio_test_axi_mark_t *mark, sdio_test_window_t *out)
{
    (void)mark;
    (void)out;
}

#endif
