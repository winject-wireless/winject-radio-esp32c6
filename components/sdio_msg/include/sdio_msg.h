#pragma once

// SDIO link messages (docs/sdio-link.md): a 4-byte header {plane, reserved,
// len LE} followed by the m-plane or d-plane datagram and zero padding to a
// multiple of 4. Shared by the P4 host and the C6 slave; no IDF dependencies.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SDIO_MSG_HDR_LEN 4u
#define SDIO_MSG_PLANE_MPLANE 1u
#define SDIO_MSG_PLANE_DPLANE 2u
// Largest datagram: a received MPDU (1500) plus its 4-byte trailer.
#define SDIO_MSG_MAX_PAYLOAD 1504u
#define SDIO_MSG_ALIGN4(n) (((n) + 3u) & ~3u)
#define SDIO_MSG_SIZE(len) SDIO_MSG_ALIGN4(SDIO_MSG_HDR_LEN + (len))
#define SDIO_MSG_MAX_SIZE SDIO_MSG_SIZE(SDIO_MSG_MAX_PAYLOAD)

// Slave scratch register the C6 sets once its link is up; the P4 waits for it
// after resetting the C6.
#define SDIO_LINK_READY_REG 0
#define SDIO_LINK_READY_VALUE 0xc6

// Write one message into `out` (room for SDIO_MSG_SIZE(len) bytes). Returns its
// size, or 0 if `plane` is unknown or `len` is above SDIO_MSG_MAX_PAYLOAD.
size_t sdio_msg_encode(uint8_t *out, uint8_t plane, const void *payload, uint16_t len);

typedef void (*sdio_msg_cb_t)(void *ctx, uint8_t plane, const uint8_t *payload, uint16_t len);

typedef enum {
    SDIO_MSG_PARSE_OK,       // every complete message delivered; tail (if any) is a partial message
    SDIO_MSG_PARSE_BAD,      // unknown plane or len above the maximum; rest of the buffer dropped
} sdio_msg_parse_result_t;

// Deliver each complete message in buf[0..len) to `cb`. A header with plane 0
// and len 0 ends the transfer (the rest is padding). Returns the number of
// bytes consumed: the caller keeps buf[consumed..len), a message cut off at the
// end of the buffer, and prepends it to the next transfer. On BAD, consumed
// is len.
size_t sdio_msg_parse(const uint8_t *buf, size_t len, sdio_msg_cb_t cb, void *ctx,
                      sdio_msg_parse_result_t *result);

#ifdef __cplusplus
}
#endif
