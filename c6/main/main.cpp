// winject radio on the ESP32-C6: raw 802.11 inject / promiscuous receive with
// the m-plane and d-plane on the SDIO link to the P4 (docs/sdio-link.md).
// Same radio, m-plane and settings code as winject-radio-esp32; the Ethernet
// network manager, HTTP OTA and Ethernet tests are not here.

#include "config.h"
#include "device_backend_esp.h"
#include "frame.h"
#include "mplane_commands.h"
#include "packet.h"
#include "radio_backend_esp.h"
#include "sdio_console.h"
#include "sdio_endpoint.h"
#include "sdio_link_slave.h"
#include "sdio_ota_server.h"
#include "settings.h"
#include "test_backend_esp.h"
#include "uart_console.h"
#include "wifi.h"

#include <optional>
#include <stdio.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char* TAG = "winject";

static std::optional<radio_backend_esp> g_radio_backend;
static std::optional<test_backend_esp> g_test_backend;
static std::optional<device_backend_esp> g_device_backend;
static std::optional<mplane_commands> g_commands;

static void on_link_message(void* ctx, uint8_t plane, const uint8_t* payload, uint16_t len)
{
    (void)ctx;
    static bool heard = false;
    if (!heard)
    {
        heard = true;
        sdio_ota_server_confirm_image();
    }
    if (plane == SDIO_MSG_PLANE_DPLANE)
    {
        sdio_endpoint::instance().on_dplane(payload, len);
    }
    else if (plane == SDIO_MSG_PLANE_OTA)
    {
        sdio_ota_server_on_message(payload, len);
    }
    else
    {
        sdio_console::instance().on_request(payload, len);
    }
}

static void start_console(mplane_radio_backend* radio, mplane_test_backend* test)
{
    g_device_backend.emplace(settings::instance(), radio);
    g_commands.emplace(*g_device_backend, radio, test);
    if (!sdio_console::instance().start(*g_commands))
    {
        ESP_LOGE(TAG, "console start failed");
    }
}

// Sizes queues / pools / driver rings from the boot tune and brings up the
// radio and d-plane. false = radio unusable (console still starts).
static bool start_radio(const settings_snapshot& boot)
{
    const tune_config& tune = boot.tune;
    wifi& radio = wifi::instance();
    if (!packet_allocator::rx().init(tune.rx_queue_sz) ||
        !radio.tx().init(tune.tx_queue_sz) || !radio.rx().init(tune.rx_queue_sz))
    {
        ESP_LOGE(TAG, "radio queue init failed (tx=%u rx=%u)",
                 static_cast<unsigned>(tune.tx_queue_sz),
                 static_cast<unsigned>(tune.rx_queue_sz));
        return false;
    }
    if (!radio.initialize(tune.wifi_tx_ring_sz, tune.wifi_rx_ring_sz))
    {
        ESP_LOGE(TAG, "wifi radio init failed");
        return false;
    }
    if (!frameBegin())
    {
        ESP_LOGE(TAG, "802.11 frame init failed");
        return false;
    }

    g_radio_backend.emplace(radio);
    if (device_backend_esp::apply_radio(*g_radio_backend, boot.radio, boot.rx_filter) !=
        mplane_status::ok)
    {
        ESP_LOGE(TAG, "boot radio config apply failed (channel=%u %s)",
                 static_cast<unsigned>(boot.radio.channel), boot.radio.modulation);
    }

    if (!radio.tx().start())
    {
        return false;
    }
    sdio_endpoint::instance().set_sink(radio.tx());
    if (!sdio_endpoint::instance().start_forward(radio.rx()))
    {
        ESP_LOGE(TAG, "d-plane forward start failed");
    }
    return true;
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "winject-esp32c6 starting  reset %d", (int)esp_reset_reason());

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        err = nvs_flash_erase();
        if (err == ESP_OK)
        {
            err = nvs_flash_init();
        }
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "nvs init failed: %s (continuing)", esp_err_to_name(err));
    }
    // The WiFi driver needs a default netif and event loop even though no IP
    // traffic uses them.
    if (esp_netif_init() != ESP_OK || esp_event_loop_create_default() != ESP_OK)
    {
        ESP_LOGE(TAG, "netif / event loop init failed");
    }

    uart_console_start();

    settings& store = settings::instance();
    store.load_boot();
    const settings_snapshot& boot = store.boot();

    // The link and the update path come up first so the P4 can reach the
    // m-plane and replace the firmware even if the radio fails.
    if (!sdio_ota_server_start())
    {
        ESP_LOGE(TAG, "OTA server start failed");
    }
    sdio_link_slave_config_t link = SDIO_LINK_SLAVE_CONFIG_DEFAULT();
    link.rx_task_prio = SDIO_LINK_RX_TASK_PRIO;
    if (sdio_link_slave_start(&link, on_link_message, nullptr) != ESP_OK)
    {
        ESP_LOGE(TAG, "SDIO link start failed");
    }

    frameSetMode(store.boot_mode());
    if (store.boot_mode() == WINJECT_MODE_OTA)
    {
        ESP_LOGI(TAG, "OTA mode: m-plane only, radio off");
        start_console(nullptr, nullptr);
        return;
    }

    const bool radio_ok = start_radio(boot);
    g_test_backend.emplace(wifi::instance());
    start_console(radio_ok ? &*g_radio_backend : nullptr, &*g_test_backend);
    if (!radio_ok)
    {
        ESP_LOGW(TAG, "running degraded: m-plane up, radio down");
    }
    printf("WINJECT_C6_READY\n");
}
