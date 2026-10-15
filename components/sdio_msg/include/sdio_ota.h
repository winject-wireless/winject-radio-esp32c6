#pragma once

// C6 firmware update over the SDIO link (plane SDIO_MSG_PLANE_OTA). The P4
// sends one request and waits for its reply before the next; all fields are
// little-endian.
//
//   BEGIN  arg = image size          C6: esp_ota_begin on the inactive slot
//   DATA   arg = offset, + bytes     C6: esp_ota_write if offset is the next
//                                    expected one; a repeated chunk (offset
//                                    below it, e.g. a retry after a lost
//                                    reply) is acknowledged without writing
//   END                              C6: esp_ota_end (checks the image) and
//                                    esp_ota_set_boot_partition
//   ABORT                            C6: esp_ota_abort
//
// Reply: same op, status 0 = ok / 1 = error (err = esp_err_t), arg = the next
// offset the C6 expects. The new image boots after the P4 resets the C6; it is
// marked valid once it hears from the P4 over the link, otherwise the
// bootloader rolls back on the following reset.

#include <stdint.h>

#define SDIO_OTA_BEGIN 1u
#define SDIO_OTA_DATA 2u
#define SDIO_OTA_END 3u
#define SDIO_OTA_ABORT 4u

#define SDIO_OTA_STATUS_OK 0u
#define SDIO_OTA_STATUS_ERROR 1u

#define SDIO_OTA_CHUNK_MAX 1400u

typedef struct __attribute__((packed)) {
    uint8_t op;
    uint8_t reserved[3];
    uint32_t arg;
} sdio_ota_req_t;

typedef struct __attribute__((packed)) {
    uint8_t op;
    uint8_t status;
    uint16_t reserved;
    uint32_t arg;
    int32_t err;
} sdio_ota_reply_t;
