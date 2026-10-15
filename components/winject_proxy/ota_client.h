#pragma once

// winject_proxy's HTTP server: status page and C6 firmware update over the
// SDIO OTA plane.

#include <stdint.h>

#include "esp_err.h"

esp_err_t ota_client_start_http(uint16_t port);

// Link receive task: an OTA-plane reply from the C6.
void ota_client_on_reply(const uint8_t *payload, uint16_t len);
