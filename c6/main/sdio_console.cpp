#include "sdio_console.h"

#include "mplane_commands.h"
#include "sdio_link_slave.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/task.h"

static const char* TAG = "console";

sdio_console& sdio_console::instance()
{
    static sdio_console inst;
    return inst;
}

bool sdio_console::start(mplane_commands& commands)
{
    if (queue_ != nullptr)
    {
        return true;
    }
    commands_ = &commands;
    queue_ = xQueueCreate(MPLANE_QUEUE_LEN, sizeof(request));
    if (queue_ == nullptr)
    {
        ESP_LOGE(TAG, "queue create failed");
        return false;
    }
    if (xTaskCreatePinnedToCore(task, "mplane", MPLANE_TASK_STACK, this, MPLANE_TASK_PRIO,
                                nullptr, APP_TASK_CORE) != pdPASS)
    {
        ESP_LOGE(TAG, "task create failed");
        return false;
    }
    ESP_LOGI(TAG, "m-plane over SDIO");
    return true;
}

void sdio_console::on_request(const uint8_t* payload, uint16_t len)
{
    QueueHandle_t q = queue_;
    if (q == nullptr || len == 0)
    {
        return;
    }
    // The request struct is large; build it in place in a static (only the
    // link receive task calls this).
    static request req;
    req.len = len < k_request_max ? len : k_request_max - 1;
    memcpy(req.text, payload, req.len);
    if (xQueueSend(q, &req, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "m-plane queue full, request dropped");
    }
}

void sdio_console::write(const char* data, size_t n)
{
    size_t off = 0;
    while (off < n)
    {
        if (reply_len_ >= k_reply_max)
        {
            flush_reply();
        }
        const size_t room = k_reply_max - reply_len_;
        const size_t chunk = n - off < room ? n - off : room;
        memcpy(reply_ + reply_len_, data + off, chunk);
        reply_len_ += chunk;
        off += chunk;
    }
}

void sdio_console::flush_reply()
{
    size_t off = 0;
    while (off < reply_len_)
    {
        size_t n = reply_len_ - off;
        if (n > k_message_max)
        {
            // Split after the last newline that fits; a longer line is cut.
            n = k_message_max;
            for (size_t i = k_message_max; i > 0; i--)
            {
                if (reply_[off + i - 1] == '\n')
                {
                    n = i;
                    break;
                }
            }
        }
        const esp_err_t err = sdio_link_slave_send(SDIO_MSG_PLANE_MPLANE, reply_ + off,
                                                   static_cast<uint16_t>(n),
                                                   pdMS_TO_TICKS(1000));
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "reply send failed: 0x%x", err);
            break;
        }
        off += n;
    }
    reply_len_ = 0;
}

void sdio_console::run()
{
    static request req;
    for (;;)
    {
        if (xQueueReceive(queue_, &req, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }
        memcpy(buf_, req.text, req.len);
        buf_[req.len] = '\0';
        reply_len_ = 0;
        commands_->handle_text(buf_, *this);
        flush_reply();
    }
}

void sdio_console::task(void* arg)
{
    static_cast<sdio_console*>(arg)->run();
}
