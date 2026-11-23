#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdio_msg.h"
#include "sdio_test.h"

#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
            exit(1);                                                     \
        }                                                                \
    } while (0)

struct got {
    int n;
    uint8_t plane[16];
    uint16_t len[16];
    uint8_t first[16];
};

static void cb(void *ctx, uint8_t plane, const uint8_t *payload, uint16_t len)
{
    struct got *g = ctx;
    g->plane[g->n] = plane;
    g->len[g->n] = len;
    g->first[g->n] = len ? payload[0] : 0;
    g->n++;
}

static void test_encode(void)
{
    uint8_t out[SDIO_MSG_MAX_SIZE + 8];
    const uint8_t p[5] = {1, 2, 3, 4, 5};
    memset(out, 0xee, sizeof(out));
    CHECK(sdio_msg_encode(out, SDIO_MSG_PLANE_DPLANE, p, 5) == 12);
    CHECK(out[0] == 2 && out[1] == 0 && out[2] == 5 && out[3] == 0);
    CHECK(memcmp(out + 4, p, 5) == 0);
    CHECK(out[9] == 0 && out[10] == 0 && out[11] == 0);
    CHECK(out[12] == 0xee);
    CHECK(sdio_msg_encode(out, SDIO_MSG_PLANE_MPLANE, NULL, 0) == 4);
    CHECK(sdio_msg_encode(out, SDIO_MSG_PLANE_OTA, p, 5) == 12);
    CHECK(sdio_msg_encode(out, SDIO_MSG_PLANE_TEST, p, 5) == 12);
    CHECK(sdio_msg_encode(out, 5, p, 5) == 0);
    CHECK(sdio_msg_encode(out, 0, p, 5) == 0);
    static uint8_t big[SDIO_MSG_MAX_PAYLOAD + 1];
    CHECK(sdio_msg_encode(out, SDIO_MSG_PLANE_DPLANE, big, SDIO_MSG_MAX_PAYLOAD) == SDIO_MSG_MAX_SIZE);
    CHECK(out[2] == (SDIO_MSG_MAX_PAYLOAD & 0xff) && out[3] == (SDIO_MSG_MAX_PAYLOAD >> 8));
    CHECK(sdio_msg_encode(out, SDIO_MSG_PLANE_DPLANE, big, SDIO_MSG_MAX_PAYLOAD + 1) == 0);
}

static void test_parse_batch_and_tail(void)
{
    uint8_t buf[4096];
    uint8_t p[1400];
    size_t off = 0;
    p[0] = 0xa1;
    off += sdio_msg_encode(buf + off, SDIO_MSG_PLANE_DPLANE, p, 1400);
    p[0] = 0xb2;
    off += sdio_msg_encode(buf + off, SDIO_MSG_PLANE_MPLANE, p, 7);
    const size_t second_end = off;
    p[0] = 0xc3;
    off += sdio_msg_encode(buf + off, SDIO_MSG_PLANE_DPLANE, p, 1000);

    struct got g = {0};
    sdio_msg_parse_result_t r;
    // Cut the third message short: the first two are delivered, the tail kept.
    size_t used = sdio_msg_parse(buf, off - 10, cb, &g, &r);
    CHECK(r == SDIO_MSG_PARSE_OK);
    CHECK(used == second_end);
    CHECK(g.n == 2);
    CHECK(g.plane[0] == 2 && g.len[0] == 1400 && g.first[0] == 0xa1);
    CHECK(g.plane[1] == 1 && g.len[1] == 7 && g.first[1] == 0xb2);
    // The full buffer from the tail on.
    used = sdio_msg_parse(buf + second_end, off - second_end, cb, &g, &r);
    CHECK(r == SDIO_MSG_PARSE_OK && used == off - second_end);
    CHECK(g.n == 3 && g.len[2] == 1000 && g.first[2] == 0xc3);
    // Fewer bytes than a header: nothing consumed.
    g.n = 0;
    CHECK(sdio_msg_parse(buf, 3, cb, &g, &r) == 0 && g.n == 0 && r == SDIO_MSG_PARSE_OK);
}

static void test_parse_terminator_and_bad(void)
{
    uint8_t buf[64] = {0};
    uint8_t p[3] = {9, 9, 9};
    size_t off = sdio_msg_encode(buf, SDIO_MSG_PLANE_MPLANE, p, 3);
    // Zero header then padding: everything consumed, one message.
    struct got g = {0};
    sdio_msg_parse_result_t r;
    CHECK(sdio_msg_parse(buf, 64, cb, &g, &r) == 64);
    CHECK(r == SDIO_MSG_PARSE_OK && g.n == 1);
    // Unknown plane after a good message.
    buf[off] = 7;
    buf[off + 2] = 4;
    g.n = 0;
    CHECK(sdio_msg_parse(buf, 64, cb, &g, &r) == 64);
    CHECK(r == SDIO_MSG_PARSE_BAD && g.n == 1);
    // Length above the maximum.
    buf[off] = SDIO_MSG_PLANE_DPLANE;
    buf[off + 2] = 0xff;
    buf[off + 3] = 0xff;
    g.n = 0;
    CHECK(sdio_msg_parse(buf, 64, cb, &g, &r) == 64);
    CHECK(r == SDIO_MSG_PARSE_BAD && g.n == 1);
}

static void rx(sdio_test_rx_t *r, uint16_t run, uint32_t seq, uint16_t len, int64_t us)
{
    uint8_t buf[64] = {0};
    sdio_test_packet_set(buf, run, seq);
    sdio_test_rx_packet(r, buf, len, us);
}

static void test_rx_counter(void)
{
    CHECK(sizeof(sdio_test_packet_t) == 8);
    CHECK(sizeof(sdio_test_bw_tx_t) == 16);
    CHECK(sizeof(sdio_test_get_rx_stats_t) == 8);
    sdio_test_rx_t r;
    sdio_test_rx_reset(&r, 7);
    rx(&r, 7, 0, 64, 1000);
    rx(&r, 7, 1, 64, 1100);
    rx(&r, 6, 2, 64, 1150);  // other run: ignored
    rx(&r, 7, 4, 64, 1200);  // 2 and 3 missing
    CHECK(r.c.pkts == 3 && r.c.lost == 2 && r.c.reorder == 0 && r.c.bytes == 192);
    rx(&r, 7, 2, 64, 1300);  // late
    CHECK(r.c.pkts == 4 && r.c.lost == 1 && r.c.reorder == 1);
    rx(&r, 7, 4, 64, 1400);  // repeat: 3 is still missing
    CHECK(r.c.reorder == 2 && r.c.lost == 1 && r.c.span_us == 400);
    rx(&r, 7, 2, 64, 1410);  // repeat of a late one
    CHECK(r.c.reorder == 3 && r.c.lost == 1);
    rx(&r, 7, 3, 64, 1420);  // late
    CHECK(r.c.reorder == 4 && r.c.lost == 0);
    rx(&r, 7, 5, 4, 1500);   // too short
    CHECK(r.c.bad == 1 && r.c.pkts == 7);
    // A gap wider than the window, then a late packet from far behind: too
    // old to tell, so it does not lower lost; one inside the window does.
    rx(&r, 7, 5 + SDIO_TEST_RX_WINDOW + 10, 64, 1600);
    CHECK(r.c.lost == SDIO_TEST_RX_WINDOW + 10);
    rx(&r, 7, 6, 64, 1700);
    CHECK(r.c.lost == SDIO_TEST_RX_WINDOW + 10);
    rx(&r, 7, 5 + SDIO_TEST_RX_WINDOW + 5, 64, 1800);
    CHECK(r.c.lost == SDIO_TEST_RX_WINDOW + 9);
    rx(&r, 7, 5 + SDIO_TEST_RX_WINDOW + 5, 64, 1900);
    CHECK(r.c.lost == SDIO_TEST_RX_WINDOW + 9);
    // First packet not seq 0: the ones before it count as lost.
    sdio_test_rx_reset(&r, 8);
    rx(&r, 8, 3, 64, 0);
    CHECK(r.c.pkts == 1 && r.c.lost == 3 && r.c.span_us == 0);
}

int main(void)
{
    test_rx_counter();
    test_encode();
    test_parse_batch_and_tail();
    test_parse_terminator_and_bad();
    printf("sdio_msg: ok\n");
    return 0;
}
