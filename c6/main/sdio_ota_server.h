#ifndef WINJECT_SDIO_OTA_SERVER_H_
#define WINJECT_SDIO_OTA_SERVER_H_

#include <stdint.h>

// C6 side of the firmware update over SDIO (components/sdio_msg/include/
// sdio_ota.h). Flash erase/write blocks for tens of ms, so requests are
// handled in their own task, not in the link receive task.
bool sdio_ota_server_start();

// Link receive task: one OTA message from the P4 (copied).
void sdio_ota_server_on_message(const uint8_t* payload, uint16_t len);

// Called on the first message from the P4 after boot: a new image that got
// this far is marked valid (cancels the bootloader's rollback).
void sdio_ota_server_confirm_image();

#endif  // WINJECT_SDIO_OTA_SERVER_H_
