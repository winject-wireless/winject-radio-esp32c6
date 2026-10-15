#include "sdio_msg.h"

#include <string.h>

static bool plane_known(uint8_t plane)
{
    return plane == SDIO_MSG_PLANE_MPLANE || plane == SDIO_MSG_PLANE_DPLANE;
}

size_t sdio_msg_encode(uint8_t *out, uint8_t plane, const void *payload, uint16_t len)
{
    if (!plane_known(plane) || len > SDIO_MSG_MAX_PAYLOAD) {
        return 0;
    }
    const size_t size = SDIO_MSG_SIZE(len);
    out[0] = plane;
    out[1] = 0;
    out[2] = (uint8_t)(len & 0xff);
    out[3] = (uint8_t)(len >> 8);
    if (len) {
        memcpy(out + SDIO_MSG_HDR_LEN, payload, len);
    }
    memset(out + SDIO_MSG_HDR_LEN + len, 0, size - SDIO_MSG_HDR_LEN - len);
    return size;
}

size_t sdio_msg_parse(const uint8_t *buf, size_t len, sdio_msg_cb_t cb, void *ctx,
                      sdio_msg_parse_result_t *result)
{
    size_t off = 0;
    *result = SDIO_MSG_PARSE_OK;
    while (len - off >= SDIO_MSG_HDR_LEN) {
        const uint8_t plane = buf[off];
        const uint16_t plen = (uint16_t)(buf[off + 2] | (buf[off + 3] << 8));
        if (plane == 0 && plen == 0) {
            return len;
        }
        if (!plane_known(plane) || plen > SDIO_MSG_MAX_PAYLOAD) {
            *result = SDIO_MSG_PARSE_BAD;
            return len;
        }
        const size_t size = SDIO_MSG_SIZE(plen);
        if (len - off < size) {
            break;
        }
        cb(ctx, plane, buf + off + SDIO_MSG_HDR_LEN, plen);
        off += size;
    }
    return off;
}
