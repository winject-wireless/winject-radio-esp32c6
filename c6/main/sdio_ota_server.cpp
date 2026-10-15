#include "sdio_ota_server.h"

#include "sdio_link_slave.h"
#include "sdio_ota.h"

#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char* TAG = "ota";

namespace
{

struct request
{
    uint16_t len;
    uint8_t data[sizeof(sdio_ota_req_t) + SDIO_OTA_CHUNK_MAX];
};

QueueHandle_t s_queue;
esp_ota_handle_t s_handle;
const esp_partition_t* s_target;
uint32_t s_size;
uint32_t s_next;  // next offset expected
bool s_active;

void reply(uint8_t op, esp_err_t err)
{
    sdio_ota_reply_t r = {};
    r.op = op;
    r.status = err == ESP_OK ? SDIO_OTA_STATUS_OK : SDIO_OTA_STATUS_ERROR;
    r.arg = s_next;
    r.err = err;
    sdio_link_slave_send(SDIO_MSG_PLANE_OTA, &r, sizeof(r), pdMS_TO_TICKS(500));
}

void abort_update()
{
    if (s_active)
    {
        esp_ota_abort(s_handle);
        s_active = false;
    }
}

void handle(const uint8_t* data, uint16_t len)
{
    sdio_ota_req_t req;
    if (len < sizeof(req))
    {
        return;
    }
    memcpy(&req, data, sizeof(req));
    const uint8_t* body = data + sizeof(req);
    const uint16_t body_len = len - sizeof(req);

    switch (req.op)
    {
        case SDIO_OTA_BEGIN:
        {
            abort_update();
            s_next = 0;
            s_target = esp_ota_get_next_update_partition(nullptr);
            if (s_target == nullptr || req.arg == 0 || req.arg > s_target->size)
            {
                ESP_LOGE(TAG, "begin: image %u B does not fit", static_cast<unsigned>(req.arg));
                reply(req.op, ESP_ERR_INVALID_SIZE);
                return;
            }
            // Erase as the image arrives instead of the whole slot up front,
            // so each reply comes within one sector erase.
            const esp_err_t err = esp_ota_begin(s_target, OTA_WITH_SEQUENTIAL_WRITES, &s_handle);
            if (err == ESP_OK)
            {
                s_active = true;
                s_size = req.arg;
                ESP_LOGI(TAG, "begin: %u B -> %s", static_cast<unsigned>(s_size), s_target->label);
            }
            reply(req.op, err);
            return;
        }
        case SDIO_OTA_DATA:
        {
            if (!s_active)
            {
                reply(req.op, ESP_ERR_INVALID_STATE);
                return;
            }
            if (req.arg < s_next)
            {
                reply(req.op, ESP_OK);  // repeat of a chunk already written
                return;
            }
            if (req.arg > s_next || s_next + body_len > s_size)
            {
                reply(req.op, ESP_ERR_INVALID_ARG);
                return;
            }
            const esp_err_t err = esp_ota_write(s_handle, body, body_len);
            if (err == ESP_OK)
            {
                s_next += body_len;
            }
            else
            {
                ESP_LOGE(TAG, "write at %u: %s", static_cast<unsigned>(req.arg),
                         esp_err_to_name(err));
                abort_update();
            }
            reply(req.op, err);
            return;
        }
        case SDIO_OTA_END:
        {
            if (!s_active || s_next != s_size)
            {
                reply(req.op, ESP_ERR_INVALID_STATE);
                return;
            }
            s_active = false;
            esp_err_t err = esp_ota_end(s_handle);  // checks the image
            if (err == ESP_OK)
            {
                err = esp_ota_set_boot_partition(s_target);
            }
            ESP_LOGI(TAG, "end: %s", esp_err_to_name(err));
            reply(req.op, err);
            return;
        }
        case SDIO_OTA_ABORT:
            abort_update();
            s_next = 0;
            reply(req.op, ESP_OK);
            return;
        default:
            reply(req.op, ESP_ERR_NOT_SUPPORTED);
            return;
    }
}

void task(void* arg)
{
    (void)arg;
    static request req;
    for (;;)
    {
        if (xQueueReceive(s_queue, &req, portMAX_DELAY) == pdTRUE)
        {
            handle(req.data, req.len);
        }
    }
}

}  // namespace

bool sdio_ota_server_start()
{
    // The P4 waits for each reply, so one request in flight (two for a retry).
    s_queue = xQueueCreate(2, sizeof(request));
    return s_queue != nullptr &&
           xTaskCreatePinnedToCore(task, "ota", 4096, nullptr, 4, nullptr, 0) == pdPASS;
}

void sdio_ota_server_on_message(const uint8_t* payload, uint16_t len)
{
    static request req;  // only the link receive task calls this
    if (s_queue == nullptr || len > sizeof(req.data))
    {
        return;
    }
    req.len = len;
    memcpy(req.data, payload, len);
    if (xQueueSend(s_queue, &req, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "queue full, request dropped (the P4 retries)");
    }
}

void sdio_ota_server_confirm_image()
{
    esp_ota_img_states_t state;
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY)
    {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new image on %s confirmed (link to the P4 works)", running->label);
    }
}
