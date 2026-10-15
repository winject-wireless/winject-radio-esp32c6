#pragma once

// C6 side of the SDIO link (docs/sdio-link.md). The SDIO slave runs in stream
// mode so the P4 can read every queued message in one transfer.

#include <stdint.h>

#include "esp_err.h"
#include "sdio_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

// Slave receive buffer size; the host must use the same value
// (essl recv_buffer_size). Holds one SDIO_MSG_MAX_SIZE message.
#define SDIO_LINK_SLAVE_RX_BUF_SIZE 1536

typedef struct {
    int rx_bufs;         // receive buffers loaded for the host (P4 -> C6)
    int tx_bufs;         // send buffers (C6 -> P4)
    int rx_task_prio;
    int rx_task_stack;
} sdio_link_slave_config_t;

// 4 receive buffers: in the link bench, 16 let the P4 write longer bursts and
// the C6's injection slowed more (1443-1712 vs 1750-1805 pps with the link
// saturated both ways), for almost no gain in P4 -> C6 throughput (50.3 vs
// 48.6 Mbps). Those runs injected constant-0xa5 frames, which slow CCA by
// themselves (docs/open-issues.md, issue 1), so the gap may be smaller with
// real payloads.
#define SDIO_LINK_SLAVE_CONFIG_DEFAULT() \
    { .rx_bufs = 4, .tx_bufs = 16, .rx_task_prio = 19, .rx_task_stack = 4096 }

typedef struct {
    uint32_t rx_msgs, rx_bytes, rx_bad;   // P4 -> C6 messages delivered / malformed transfers
    uint32_t tx_msgs, tx_bytes, tx_timeout;
} sdio_link_slave_stats_t;

// Start the SDIO slave and the receive task. `cb` runs in the receive task for
// every message from the P4; the payload is valid only during the call.
esp_err_t sdio_link_slave_start(const sdio_link_slave_config_t *config, sdio_msg_cb_t cb, void *ctx);

// Queue one message for the P4. Copies the payload. Blocks up to `timeout_ticks`
// for a free send buffer (the P4 reads them back); ESP_ERR_TIMEOUT if none.
// Safe from several tasks.
esp_err_t sdio_link_slave_send(uint8_t plane, const void *payload, uint16_t len,
                               uint32_t timeout_ticks);

void sdio_link_slave_get_stats(sdio_link_slave_stats_t *out);

#ifdef __cplusplus
}
#endif
