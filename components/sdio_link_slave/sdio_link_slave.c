#include "sdio_link_slave.h"

#include <string.h>

#include "driver/sdio_slave.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "sdio_link";

#define TX_BUF_SIZE SDIO_LINK_SLAVE_RX_BUF_SIZE
// A host transfer longer than one receive buffer arrives as several buffers
// (ESP_ERR_NOT_FINISHED); they are joined here before parsing.
#define REASM_SIZE (4 * SDIO_LINK_SLAVE_RX_BUF_SIZE)

static sdio_msg_cb_t s_cb;
static void *s_ctx;
static uint8_t **s_tx_bufs;
static QueueHandle_t s_tx_free;  // indices of free send buffers
static SemaphoreHandle_t s_tx_lock;
static uint8_t *s_reasm;
static portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;
static sdio_link_slave_stats_t s_stats;

static void count_rx(void *ctx, uint8_t plane, const uint8_t *payload, uint16_t len)
{
    taskENTER_CRITICAL(&s_stats_mux);
    s_stats.rx_msgs++;
    s_stats.rx_bytes += len;
    taskEXIT_CRITICAL(&s_stats_mux);
    s_cb(s_ctx, plane, payload, len);
}

static void parse(const uint8_t *buf, size_t len)
{
    sdio_msg_parse_result_t r;
    size_t used = sdio_msg_parse(buf, len, count_rx, NULL, &r);
    // One host transfer always holds whole messages, so a tail is malformed too.
    if (r != SDIO_MSG_PARSE_OK || used != len) {
        taskENTER_CRITICAL(&s_stats_mux);
        s_stats.rx_bad++;
        taskEXIT_CRITICAL(&s_stats_mux);
    }
}

static void rx_task(void *arg)
{
    size_t reasm_len = 0;
    bool reasm_overflow = false;
    for (;;) {
        sdio_slave_buf_handle_t h;
        esp_err_t ret = sdio_slave_recv_packet(&h, portMAX_DELAY);
        if (ret != ESP_OK && ret != ESP_ERR_NOT_FINISHED) {
            ESP_LOGE(TAG, "recv: 0x%x", ret);
            continue;
        }
        size_t len;
        uint8_t *buf = sdio_slave_recv_get_buf(h, &len);
        if (ret == ESP_OK && reasm_len == 0) {
            parse(buf, len);  // common case: the transfer fits one buffer
        } else {
            if (reasm_len + len <= REASM_SIZE) {
                memcpy(s_reasm + reasm_len, buf, len);
                reasm_len += len;
            } else {
                reasm_overflow = true;
            }
            if (ret == ESP_OK) {
                if (reasm_overflow) {
                    taskENTER_CRITICAL(&s_stats_mux);
                    s_stats.rx_bad++;
                    taskEXIT_CRITICAL(&s_stats_mux);
                } else {
                    parse(s_reasm, reasm_len);
                }
                reasm_len = 0;
                reasm_overflow = false;
            }
        }
        ESP_ERROR_CHECK(sdio_slave_recv_load_buf(h));
    }
}

esp_err_t sdio_link_slave_start(const sdio_link_slave_config_t *config, sdio_msg_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_ctx = ctx;
    s_tx_free = xQueueCreate(config->tx_bufs, sizeof(int));
    s_tx_lock = xSemaphoreCreateMutex();
    s_tx_bufs = calloc(config->tx_bufs, sizeof(uint8_t *));
    s_reasm = malloc(REASM_SIZE);
    if (!s_tx_free || !s_tx_lock || !s_tx_bufs || !s_reasm) {
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < config->tx_bufs; i++) {
        s_tx_bufs[i] = heap_caps_malloc(TX_BUF_SIZE, MALLOC_CAP_DMA);
        if (!s_tx_bufs[i]) {
            return ESP_ERR_NO_MEM;
        }
        xQueueSend(s_tx_free, &i, 0);
    }

    sdio_slave_config_t sc = {
        .sending_mode = SDIO_SLAVE_SEND_STREAM,
        .send_queue_size = config->tx_bufs,
        .recv_buffer_size = SDIO_LINK_SLAVE_RX_BUF_SIZE,
    };
    esp_err_t ret = sdio_slave_initialize(&sc);
    if (ret != ESP_OK) {
        return ret;
    }
    for (int i = 0; i < config->rx_bufs; i++) {
        uint8_t *b = heap_caps_malloc(SDIO_LINK_SLAVE_RX_BUF_SIZE, MALLOC_CAP_DMA);
        sdio_slave_buf_handle_t h = b ? sdio_slave_recv_register_buf(b) : NULL;
        if (!h) {
            return ESP_ERR_NO_MEM;
        }
        ESP_ERROR_CHECK(sdio_slave_recv_load_buf(h));
    }
    sdio_slave_set_host_intena(SDIO_SLAVE_HOSTINT_SEND_NEW_PACKET);
    ret = sdio_slave_start();
    if (ret != ESP_OK) {
        return ret;
    }
    // Tell the host the link is up (it may have enumerated the ROM's slave
    // before this firmware re-initialised it).
    sdio_slave_write_reg(SDIO_LINK_READY_REG, SDIO_LINK_READY_VALUE);
    if (xTaskCreate(rx_task, "sdio_rx", config->rx_task_stack, NULL, config->rx_task_prio, NULL) !=
        pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "slave up: %d rx bufs, %d tx bufs of %d B", config->rx_bufs, config->tx_bufs,
             SDIO_LINK_SLAVE_RX_BUF_SIZE);
    return ESP_OK;
}

esp_err_t sdio_link_slave_send(uint8_t plane, const void *payload, uint16_t len,
                               uint32_t timeout_ticks)
{
    if (len > SDIO_MSG_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_SIZE;
    }
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    // Return buffers the host has read to the free list.
    void *done;
    while (sdio_slave_send_get_finished(&done, 0) == ESP_OK) {
        int i = (int)(intptr_t)done;
        xQueueSend(s_tx_free, &i, 0);
    }
    int idx;
    if (xQueueReceive(s_tx_free, &idx, 0) != pdTRUE) {
        if (sdio_slave_send_get_finished(&done, timeout_ticks) != ESP_OK) {
            xSemaphoreGive(s_tx_lock);
            taskENTER_CRITICAL(&s_stats_mux);
            s_stats.tx_timeout++;
            taskEXIT_CRITICAL(&s_stats_mux);
            return ESP_ERR_TIMEOUT;
        }
        idx = (int)(intptr_t)done;
    }
    size_t size = sdio_msg_encode(s_tx_bufs[idx], plane, payload, len);
    if (size == 0) {
        xQueueSend(s_tx_free, &idx, 0);
        xSemaphoreGive(s_tx_lock);
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = sdio_slave_send_queue(s_tx_bufs[idx], size, (void *)(intptr_t)idx, 0);
    if (ret != ESP_OK) {
        // The send queue has as many slots as there are buffers, so this
        // should not happen; keep the buffer.
        xQueueSend(s_tx_free, &idx, 0);
    }
    xSemaphoreGive(s_tx_lock);
    if (ret == ESP_OK) {
        taskENTER_CRITICAL(&s_stats_mux);
        s_stats.tx_msgs++;
        s_stats.tx_bytes += len;
        taskEXIT_CRITICAL(&s_stats_mux);
    }
    return ret;
}

void sdio_link_slave_get_stats(sdio_link_slave_stats_t *out)
{
    taskENTER_CRITICAL(&s_stats_mux);
    *out = s_stats;
    taskEXIT_CRITICAL(&s_stats_mux);
}
