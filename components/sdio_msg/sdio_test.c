#include "sdio_test.h"

#include <string.h>

void sdio_test_rx_reset(sdio_test_rx_t *rx, uint16_t run)
{
    memset(rx, 0, sizeof(*rx));
    rx->run = run;
}

static bool seen_get(const sdio_test_rx_t *rx, uint32_t seq)
{
    const uint32_t i = seq % SDIO_TEST_RX_WINDOW;
    return (rx->seen[i / 32] >> (i % 32)) & 1u;
}

static void seen_set(sdio_test_rx_t *rx, uint32_t seq)
{
    const uint32_t i = seq % SDIO_TEST_RX_WINDOW;
    rx->seen[i / 32] |= 1u << (i % 32);
}

static void seen_clear(sdio_test_rx_t *rx, uint32_t seq)
{
    const uint32_t i = seq % SDIO_TEST_RX_WINDOW;
    rx->seen[i / 32] &= ~(1u << (i % 32));
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
    }
    if (h.seq < rx->next_seq) {
        rx->c.reorder++;
        // Counted as a gap earlier and now arrived late, unless a repeat or
        // too old to tell.
        if (rx->next_seq - h.seq <= SDIO_TEST_RX_WINDOW && !seen_get(rx, h.seq)) {
            seen_set(rx, h.seq);
            if (rx->c.lost > 0) {
                rx->c.lost--;
            }
        }
    } else {
        const uint32_t gap = h.seq - rx->next_seq;
        rx->c.lost += gap;
        if (gap >= SDIO_TEST_RX_WINDOW) {
            memset(rx->seen, 0, sizeof(rx->seen));
        } else {
            for (uint32_t s = rx->next_seq; s != h.seq; s++) {
                seen_clear(rx, s);
            }
        }
        seen_set(rx, h.seq);
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
