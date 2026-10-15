#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdio_msg.h"

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
    CHECK(sdio_msg_encode(out, 3, p, 5) == 0);
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

int main(void)
{
    test_encode();
    test_parse_batch_and_tail();
    test_parse_terminator_and_bad();
    printf("sdio_msg: ok\n");
    return 0;
}
