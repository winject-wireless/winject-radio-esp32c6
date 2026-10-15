#include "sdio_endpoint.h"

#include "dplane_classify.h"
#include "packet.h"
#include "sdio_link_slave.h"
#include "wifi_rx.h"
#include "wifi_tx.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char* TAG = "dplane";

sdio_endpoint& sdio_endpoint::instance()
{
    static sdio_endpoint inst;
    return inst;
}

uint32_t sdio_endpoint::tx_ether_pkt() const
{
    return tx_ether_pkt_.load(std::memory_order_relaxed);
}

uint32_t sdio_endpoint::rx_ether_pkt() const
{
    return rx_ether_pkt_.load(std::memory_order_relaxed);
}

uint32_t sdio_endpoint::rx_dropped_no_peer() const
{
    return rx_dropped_no_peer_.load(std::memory_order_relaxed);
}

uint32_t sdio_endpoint::rx_dropped_send_failed() const
{
    return rx_dropped_send_failed_.load(std::memory_order_relaxed);
}

void sdio_endpoint::on_dplane(const uint8_t* payload, uint16_t len)
{
    const dplane_kind kind = dplane_classify(len);
    if (kind == dplane_kind::registration)
    {
        if (!peer_.exchange(true, std::memory_order_relaxed))
        {
            ESP_LOGI(TAG, "peer registered");
        }
        return;
    }
    wifi_tx* tx = tx_.load(std::memory_order_acquire);
    if (tx == nullptr)
    {
        return;
    }
    tx_ether_pkt_.fetch_add(1, std::memory_order_relaxed);
    if (kind == dplane_kind::invalid || len > WIFI_RADIO_INJECT_MAX)
    {
        tx->note_invalid_frame();
        return;
    }
    // The link reuses its receive buffer after this call; wifi_tx keeps the
    // frame until it has been submitted.
    uint8_t* copy = static_cast<uint8_t*>(malloc(len));
    if (copy == nullptr)
    {
        ESP_LOGW(TAG, "no memory for a %u-byte inject frame", len);
        tx->note_invalid_frame();
        return;
    }
    memcpy(copy, payload, len);
    tx->enqueue(packet::adopt_heap(copy, copy, len));
}

void sdio_endpoint::set_sink(wifi_tx& tx)
{
    tx_.store(&tx, std::memory_order_release);
    ESP_LOGI(TAG, "SDIO d-plane -> wifi_tx");
}

void sdio_endpoint::run_forward()
{
    for (;;)
    {
        packet frame = rx_->pop(portMAX_DELAY);
        if (!frame.is_valid())
        {
            continue;
        }
        rx_->on_upstream_deliver();
        if (!peer_.load(std::memory_order_relaxed))
        {
            rx_dropped_no_peer_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        const esp_err_t err =
            sdio_link_slave_send(SDIO_MSG_PLANE_DPLANE, frame.data(),
                                 static_cast<uint16_t>(frame.size()),
                                 pdMS_TO_TICKS(DPLANE_SEND_TIMEOUT_MS));
        if (err != ESP_OK)
        {
            rx_dropped_send_failed_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        rx_ether_pkt_.fetch_add(1, std::memory_order_relaxed);
    }
}

void sdio_endpoint::forward_task(void* arg)
{
    static_cast<sdio_endpoint*>(arg)->run_forward();
}

bool sdio_endpoint::start_forward(wifi_rx& rx, UBaseType_t prio, uint32_t stack_bytes)
{
    if (forward_task_ != nullptr)
    {
        return true;
    }
    rx_ = &rx;
    if (xTaskCreatePinnedToCore(forward_task, "dplane_rx", stack_bytes, this, prio,
                                &forward_task_, APP_TASK_CORE) != pdPASS)
    {
        ESP_LOGE(TAG, "forward task create failed");
        forward_task_ = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "air -> SDIO d-plane, prio=%u", static_cast<unsigned>(prio));
    return true;
}
