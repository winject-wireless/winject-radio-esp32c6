#pragma once

// CPU and memory-bus figures for the SDIO bandwidth test (sdio_test.h), shared
// by the C6 and the P4 proxy.
//
// CPU: per core, the share of a window not spent in the idle task or the
// memory probe (FreeRTOS run-time stats; needs
// CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS and CONFIG_FREERTOS_USE_TRACE_FACILITY,
// otherwise busy_permille is SDIO_TEST_CPU_UNKNOWN and the probe has no time).
//
// Memory probe: one task per core at idle priority copies between two buffers
// in a loop. Its figure is bytes copied per second of its own CPU time: with
// the bus idle it is what one core gets from that memory, and it drops as
// other masters (SDIO DMA, other cores, caches) contend for it. Compare a
// window with the link idle to one under load.
//   internal  C6: 2 x 8 KB SRAM (no data cache). P4: 2 x 8 KB of L2MEM per
//             core through the uncached alias, so every access is a bus
//             access (the lines are written back before and invalidated after).
//   psram     P4 only: 2 x 256 KB per core, cached (larger than the L2 cache).
// The buffers exist only while the probe runs.
//
// AXI monitor (sdio_test_axi.h, P4 rev 3.0+ only): set its ports with
// sdio_test_axi_set(); windows then also carry the ports' data-channel use.

#include <stdint.h>

#include "esp_err.h"
#include "sdio_test.h"
#include "sdio_test_axi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int64_t t0_us;
    uint64_t idle0[SDIO_TEST_MAX_CORES];
    uint64_t probe_rt0[SDIO_TEST_MAX_CORES];
    uint64_t probe_bytes0;
    sdio_test_axi_mark_t axi;
} sdio_test_meter_mark_t;

// Stops the running probe (if any) and starts `probe` (SDIO_TEST_PROBE_*).
// ESP_ERR_NOT_SUPPORTED: no such memory on this chip; ESP_ERR_NO_MEM: buffers
// or tasks could not be had. Either way the probe is then off. Not
// thread-safe: call from one task.
esp_err_t sdio_test_probe_set(uint8_t probe);

// Starts a window. Set the probe and the AXI ports before, not during, a
// window.
void sdio_test_meter_mark(sdio_test_meter_mark_t *mark);

// The window from `mark` to now.
void sdio_test_meter_read(const sdio_test_meter_mark_t *mark, sdio_test_window_t *out);

#ifdef __cplusplus
}
#endif
