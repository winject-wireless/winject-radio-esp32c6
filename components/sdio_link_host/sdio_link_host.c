#include "sdio_link_host.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_serial_slave_link/essl_sdio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sdio_link";

// Must equal the slave's receive buffer size (SDIO_LINK_SLAVE_RX_BUF_SIZE).
#define SLAVE_RX_BUF_SIZE 1536
// One read takes up to this much of what the C6 has queued. A multiple of the
// 512-byte block, and DMA buffers on the P4 must be cache-line aligned.
#define READ_SIZE 8192
#define DMA_ALIGN 64
#define TX_BUF_SIZE SDIO_MSG_ALIGN4(SDIO_MSG_MAX_SIZE)

static essl_handle_t s_essl;
static sdmmc_card_t *s_card;
static SemaphoreHandle_t s_lock;     // essl calls are not thread-safe
static SemaphoreHandle_t s_tx_lock;  // owns s_tx
// The receive task outranks senders; while one waits for the bus, the receive
// task steps aside after each read until that send is done (s_rx_turn).
static atomic_int s_tx_waiting;
static SemaphoreHandle_t s_rx_turn;
static sdio_msg_cb_t s_cb;
static void *s_ctx;
static uint8_t *s_read;   // DMA target
static uint8_t *s_parse;  // carried tail + read, when a message spans two reads
static uint8_t *s_tx;
static portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;
static sdio_link_host_stats_t s_stats;

#define STAT_ADD(field, v)                  \
    do {                                    \
        taskENTER_CRITICAL(&s_stats_mux);   \
        s_stats.field += (v);               \
        taskEXIT_CRITICAL(&s_stats_mux);    \
    } while (0)

static void deliver(void *ctx, uint8_t plane, const uint8_t *payload, uint16_t len)
{
    taskENTER_CRITICAL(&s_stats_mux);
    s_stats.rx_msgs++;
    s_stats.rx_bytes += len;
    taskEXIT_CRITICAL(&s_stats_mux);
    s_cb(s_ctx, plane, payload, len);
}

static void rx_task(void *arg)
{
    size_t carry = 0;  // bytes of a partial message at the start of s_parse
    for (;;) {
        esp_err_t err = essl_wait_int(s_essl, 1000);
        if (err == ESP_ERR_TIMEOUT) {
            continue;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint32_t intr = 0;
        if (essl_get_intr(s_essl, &intr, NULL, 0) == ESP_OK && intr) {
            essl_clear_intr(s_essl, intr, 0);
        }
        xSemaphoreGive(s_lock);

        // Drain: read until the C6 has nothing queued. Data queued after the
        // interrupt was cleared raises it again.
        for (;;) {
            size_t got = 0;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            // While a sender waits, read about one message so the two
            // directions alternate message for message.
            const size_t want = atomic_load(&s_tx_waiting) > 0 ? SLAVE_RX_BUF_SIZE : READ_SIZE;
            err = essl_get_packet(s_essl, s_read, want, &got, 0);
            xSemaphoreGive(s_lock);
            if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_TIMEOUT) {
                break;
            }
            if (err != ESP_OK && err != ESP_ERR_NOT_FINISHED) {
                ESP_LOGE(TAG, "read: 0x%x", err);
                STAT_ADD(rx_errors, 1);
                carry = 0;
                break;
            }
            STAT_ADD(rx_reads, 1);
            const uint8_t *buf = s_read;
            size_t len = got;
            if (carry) {
                memcpy(s_parse + carry, s_read, got);
                buf = s_parse;
                len = carry + got;
            }
            sdio_msg_parse_result_t r;
            size_t used = sdio_msg_parse(buf, len, deliver, NULL, &r);
            if (r != SDIO_MSG_PARSE_OK) {
                STAT_ADD(rx_bad, 1);
            }
            carry = len - used;
            if (carry) {
                memmove(s_parse, buf + used, carry);
            }
            if (err == ESP_OK) {
                break;
            }
            if (atomic_load(&s_tx_waiting) > 0) {
                xSemaphoreTake(s_rx_turn, pdMS_TO_TICKS(2));
            }
        }
    }
}

esp_err_t sdio_link_host_start(const sdio_link_host_config_t *config, sdio_msg_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_ctx = ctx;
    s_lock = xSemaphoreCreateMutex();
    s_tx_lock = xSemaphoreCreateMutex();
    s_rx_turn = xSemaphoreCreateBinary();
    s_read = heap_caps_aligned_alloc(DMA_ALIGN, READ_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_parse = malloc(SDIO_MSG_MAX_SIZE + READ_SIZE);
    s_tx = heap_caps_aligned_alloc(DMA_ALIGN, TX_BUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_card = calloc(1, sizeof(*s_card));
    if (!s_lock || !s_tx_lock || !s_rx_turn || !s_read || !s_parse || !s_tx || !s_card) {
        return ESP_ERR_NO_MEM;
    }

    if (config->reset_gpio >= 0) {
        gpio_config_t io = {.pin_bit_mask = 1ULL << config->reset_gpio, .mode = GPIO_MODE_OUTPUT};
        gpio_config(&io);
        gpio_set_level(config->reset_gpio, 0);
        vTaskDelay(pdMS_TO_TICKS(50));
        gpio_set_level(config->reset_gpio, 1);
    }

    esp_err_t err = sdmmc_host_init();
    if (err != ESP_OK) {
        return err;
    }
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
#ifdef CONFIG_SOC_SDMMC_USE_GPIO_MATRIX
    slot.clk = config->clk;
    slot.cmd = config->cmd;
    slot.d0 = config->d0;
    slot.d1 = config->d1;
    slot.d2 = config->d2;
    slot.d3 = config->d3;
#endif
    err = sdmmc_host_init_slot(config->slot, &slot);
    if (err != ESP_OK) {
        return err;
    }
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = config->slot;
    host.flags = SDMMC_HOST_FLAG_4BIT | SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF;
    host.max_freq_khz = config->freq_khz;

    // After reset the card can enumerate against the C6 ROM's slave, which the
    // firmware then re-initialises; retry the whole bring-up until the C6 sets
    // SDIO_LINK_READY_REG.
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(config->boot_timeout_ms);
    bool ready = false;
    while (!ready && (int32_t)(xTaskGetTickCount() - deadline) < 0) {
        if (s_essl) {
            essl_sdio_deinit_dev(s_essl);
            s_essl = NULL;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        err = sdmmc_card_init(&host, s_card);
        if (err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        essl_sdio_config_t ec = {.card = s_card, .recv_buffer_size = SLAVE_RX_BUF_SIZE};
        err = essl_sdio_init_dev(&s_essl, &ec);
        if (err != ESP_OK) {
            return err;
        }
        if (essl_init(s_essl, 1000) != ESP_OK) {
            continue;
        }
        for (int i = 0; i < 10 && !ready; i++) {
            uint8_t v = 0;
            if (essl_read_reg(s_essl, SDIO_LINK_READY_REG, &v, 100) != ESP_OK) {
                break;
            }
            ready = v == SDIO_LINK_READY_VALUE;
            if (!ready) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
    }
    if (!ready) {
        ESP_LOGE(TAG, "C6 link not ready after %d ms (last 0x%x)", config->boot_timeout_ms, err);
        return ESP_ERR_TIMEOUT;
    }
    sdmmc_host_get_real_freq(config->slot, &s_stats.real_freq_khz);
    if (xTaskCreate(rx_task, "sdio_rx", config->rx_task_stack, NULL, config->rx_task_prio, NULL) !=
        pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "host up: %d kHz (asked %d)", s_stats.real_freq_khz, config->freq_khz);
    return ESP_OK;
}

esp_err_t sdio_link_host_send(uint8_t plane, const void *payload, uint16_t len, uint32_t timeout_ms)
{
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    const size_t size = sdio_msg_encode(s_tx, plane, payload, len);
    if (size == 0) {
        xSemaphoreGive(s_tx_lock);
        return ESP_ERR_INVALID_ARG;
    }
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    esp_err_t err;
    for (;;) {
        // essl checks the slave's free buffers against its cached count and
        // reads the register only when that runs short: ESP_ERR_NOT_FOUND
        // means the C6 has no buffer yet. Wait without holding the bus so the
        // receive task keeps draining the C6, which may itself be waiting to
        // send to us.
        atomic_fetch_add(&s_tx_waiting, 1);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        err = essl_send_packet(s_essl, s_tx, size, 0);
        xSemaphoreGive(s_lock);
        atomic_fetch_sub(&s_tx_waiting, 1);
        xSemaphoreGive(s_rx_turn);
        if (err != ESP_ERR_NOT_FOUND) {
            break;
        }
        STAT_ADD(tx_credit_waits, 1);
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        vTaskDelay(1);
    }
    xSemaphoreGive(s_tx_lock);
    if (err == ESP_OK) {
        taskENTER_CRITICAL(&s_stats_mux);
        s_stats.tx_msgs++;
        s_stats.tx_bytes += len;
        taskEXIT_CRITICAL(&s_stats_mux);
    } else {
        STAT_ADD(tx_errors, 1);
    }
    return err;
}

void sdio_link_host_get_stats(sdio_link_host_stats_t *out)
{
    taskENTER_CRITICAL(&s_stats_mux);
    *out = s_stats;
    taskEXIT_CRITICAL(&s_stats_mux);
}
