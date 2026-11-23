#pragma once

// AXI performance monitor for the SDIO bandwidth test (sdio_test_axi.c). Only
// the ESP32-P4 from chip revision 3.0 has one; elsewhere sdio_test_axi_set()
// with a port returns ESP_ERR_NOT_SUPPORTED and windows report no AXI figures.

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdio_test.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t rd_sum[SDIO_TEST_AXI_UNITS];
    uint64_t wr_sum[SDIO_TEST_AXI_UNITS];
    uint32_t samples;
} sdio_test_axi_mark_t;

// This chip has the monitor (decided at run time: P4 revision >= 3.0).
bool sdio_test_axi_available(void);

// Points the two monitor units at agent[0] / agent[1] (SDIO_TEST_AXI_*; NONE
// leaves a unit off; all NONE stops the monitor). Not thread-safe.
esp_err_t sdio_test_axi_set(const uint8_t agent[SDIO_TEST_AXI_UNITS]);

// Used by sdio_test_meter_mark() / _read().
void sdio_test_axi_mark(sdio_test_axi_mark_t *mark);
void sdio_test_axi_read(const sdio_test_axi_mark_t *mark, sdio_test_window_t *out);

#ifdef __cplusplus
}
#endif
