#include "sdio_test.h"

#include <string.h>

void sdio_test_rx_reset(sdio_test_rx_t *rx, uint16_t run)
{
    memset(rx, 0, sizeof(*rx));
    rx->run = run;
}

void sdio_test_rx_packet(sdio_test_rx_t *rx, const uint8_t *payload, uint16_t len, int64_t now_us)
{
    if (len < sizeof(sdio_test_packet_t)) {
        rx->c.bad++;
        return;
    }
    sdio_test_packet_t h;
    memcpy(&h, payload, sizeof(h));
    if (h.run != rx->run) {
        return;
    }
    if (!rx->any) {
        rx->any = true;
        rx->first_us = now_us;
    } else if (h.seq < rx->next_seq) {
        rx->c.reorder++;
        if (rx->c.lost > 0) {
            rx->c.lost--;  // counted as a gap earlier, arrived late
        }
    }
    if (h.seq >= rx->next_seq) {
        rx->c.lost += h.seq - rx->next_seq;
        rx->next_seq = h.seq + 1;
    }
    rx->last_us = now_us;
    rx->c.pkts++;
    rx->c.bytes += len;
    rx->c.span_us = (uint32_t)(rx->last_us - rx->first_us);
}

void sdio_test_packet_set(uint8_t *buf, uint16_t run, uint32_t seq)
{
    const sdio_test_packet_t h = {.type = SDIO_TEST_PACKET, .run = run, .seq = seq};
    memcpy(buf, &h, sizeof(h));
}
