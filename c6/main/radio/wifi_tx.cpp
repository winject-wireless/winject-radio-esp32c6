#include "wifi_tx.h"

#include "config.h"
#include "packet.h"
#include "tx_power_map.h"
#include "wifi.h"

#include <utility>

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bfc-esp32/semaphore.hpp"

static const char* TAG = "wifi_tx";

static constexpr int64_t k_fail_log_interval_us = 1000000;

// Undocumented libphy / libpp internals of the C6 WiFi blobs (IDF 5.5.5),
// signatures read from the disassembly. Used only for the boost level above
// the public API's 84 (0.25 dBm) cap; recheck them on any IDF upgrade.
//  - phy_set_most_tpw: libphy's maximum TX power (phy_param[6]); rebuilds
//    the TX gain table from it. esp_wifi_set_max_tx_power ends here, capped 84.
//  - hal_set_tx_pwr: libpp's per-rate power table (one entry per rate code;
//    the MAC uses it per frame). hal_init_tx_pwr fills it via get_target_pwr,
//    which clamps every rate to 84. Rate codes 0..25 are the raw index.
extern "C" void phy_set_most_tpw(int8_t qdbm);
extern "C" int8_t hal_get_tx_pwr(uint32_t rate);
extern "C" void hal_set_tx_pwr(uint32_t index, int8_t qdbm);
extern "C" uint8_t phy_param[];

// Per-rate boost: DSSS, CCK and legacy OFDM up to 36 Mbps (16-QAM). 48M (0x08)
// and 54M (0x0C) are 64-QAM and keep their PHY table value. Codes are the
// wifi_phy_rate_t values (components/esp_wifi/include/esp_wifi_types_generic.h).
static bool boost_rate(uint32_t code)
{
    return code <= 0x03 || (code >= 0x05 && code <= 0x07) ||
           (code >= 0x09 && code <= 0x0B) || (code >= 0x0D && code <= 0x0F);
}

// 36 Mbps is kept identical to 24 Mbps for now (both are 16-QAM, one EVM
// group). The two rates use different PHY table slots (24M slot 6, 36M slot 8,
// which 54M shares), so copy the 24M entry to 36M after each table rebuild.
static constexpr uint32_t k_rate_24m = 0x09;
static constexpr uint32_t k_rate_36m = 0x0D;

static void match_36m_to_24m()
{
    hal_set_tx_pwr(k_rate_36m, hal_get_tx_pwr(k_rate_24m));
}

extern "C" int ieee80211_raw_frame_sanity_check(int32_t, int32_t, uint32_t,
                                                uint32_t)
{
    return 0;
}

extern "C"
{
    int hal_mac_tx_set_cca(int enable);
    void esp_rom_phy_disable_cca(void) __attribute__((weak));
    void phy_disable_cca(void) __attribute__((weak));
    void phy_enable_cca(void) __attribute__((weak));
}

wifi_tx::wifi_tx(wifi& radio) : radio(radio) {}

int8_t wifi_tx::power_dbm() const
{
    return tx_power_dbm;
}

bool wifi_tx::cca_enabled() const
{
    return cca_enabled_;
}

static tx_power_map::rate_class rate_class_for(wifi_phy_rate_t rate)
{
    using tx_power_map::rate_class;
    switch (rate)
    {
    case WIFI_PHY_RATE_1M_L:
    case WIFI_PHY_RATE_2M_L:
    case WIFI_PHY_RATE_2M_S:
        return rate_class::dsss;
    case WIFI_PHY_RATE_5M_L:
    case WIFI_PHY_RATE_5M_S:
    case WIFI_PHY_RATE_11M_L:
    case WIFI_PHY_RATE_11M_S:
        return rate_class::cck;
    case WIFI_PHY_RATE_6M:
    case WIFI_PHY_RATE_9M:
    case WIFI_PHY_RATE_12M:
    case WIFI_PHY_RATE_18M:
    case WIFI_PHY_RATE_MCS0_LGI:
    case WIFI_PHY_RATE_MCS1_LGI:
    case WIFI_PHY_RATE_MCS2_LGI:
    case WIFI_PHY_RATE_MCS0_SGI:
    case WIFI_PHY_RATE_MCS1_SGI:
    case WIFI_PHY_RATE_MCS2_SGI:
        return rate_class::ofdm_low;
    case WIFI_PHY_RATE_48M:
    case WIFI_PHY_RATE_54M:
    case WIFI_PHY_RATE_MCS5_LGI:
    case WIFI_PHY_RATE_MCS6_LGI:
    case WIFI_PHY_RATE_MCS7_LGI:
    case WIFI_PHY_RATE_MCS5_SGI:
    case WIFI_PHY_RATE_MCS6_SGI:
    case WIFI_PHY_RATE_MCS7_SGI:
        return rate_class::ofdm_64qam;
    default:
        return rate_class::ofdm_mid;
    }
}

// tx_power_dbm is normalized: dBm at the antenna connector. The calibration
// curve of the current rate group (tx_power_map) turns it into the nearest
// driver value the C6 can do, and the applied value back into dBm.
//
// Up to 84 the public API sets it (esp_wifi_set_max_tx_power, which also
// rebuilds libpp's per-rate table). Above (the boost, up to 88) libphy's
// maximum is raised, and the per-rate entries for DSSS, CCK and OFDM up to
// 36 Mbps are set to the same level (boost_rate); 48M and 54M keep their
// table value. Channel and protocol changes rebuild the table, so they call
// apply_power again.
// The country is set once before esp_wifi_start (wifi::initialize).
bool wifi_tx::apply_power()
{
    const wifi_phy_rate_t rate = radio.phy_rate();
    const tx_power_map::rate_class cls = rate_class_for(rate);
    const int q_max = (rate == WIFI_PHY_RATE_48M || rate == WIFI_PHY_RATE_54M)
                          ? tx_power_map::k_qdbm_legacy_64qam_max
                          : tx_power_map::k_qdbm_max;
    const tx_power_map::curve& curve = cal_.c[static_cast<size_t>(cls)];
    const int q = tx_power_map::qdbm_for_dbm(tx_power_dbm, curve, q_max);
    const int q_api = q < tx_power_map::k_qdbm_api_max ? q : tx_power_map::k_qdbm_api_max;

    esp_err_t err = esp_wifi_set_max_tx_power(static_cast<int8_t>(q_api));
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "set_max_tx_power %d failed: %s", q_api, esp_err_to_name(err));
        return false;
    }

    int8_t applied = 0;
    if (q > tx_power_map::k_qdbm_api_max)
    {
        phy_set_most_tpw(static_cast<int8_t>(q));
        unsigned raised = 0;
        for (uint32_t code = 0; code < 26; code++)
        {
            if (boost_rate(code))
            {
                hal_set_tx_pwr(code, static_cast<int8_t>(q));
                raised++;
            }
        }
        applied = static_cast<int8_t>(phy_param[6]);
        ESP_LOGI(TAG, "tx_power %d dBm: boost %d, %u rate entries raised", tx_power_dbm, q,
                 raised);
    }
    else
    {
        err = esp_wifi_get_max_tx_power(&applied);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "get_max_tx_power failed: %s", esp_err_to_name(err));
            applied_dbm_.store(INT8_MIN, std::memory_order_relaxed);
            return true;
        }
    }
    match_36m_to_24m();
    const int dbm = tx_power_map::dbm_for_qdbm(applied, curve);
    applied_dbm_.store(static_cast<int8_t>(dbm), std::memory_order_relaxed);
    ESP_LOGI(TAG, "tx_power %d dBm requested: driver %d (set %d) -> %d dBm", tx_power_dbm,
             applied, q, dbm);
    return true;
}

tx_power_map::cal wifi_tx::tx_cal() const
{
    return cal_;
}

bool wifi_tx::set_tx_cal(const tx_power_map::cal& cal, bool apply)
{
    if (!tx_power_map::cal_valid(cal))
    {
        return false;
    }
    const tx_power_map::cal previous = cal_;
    cal_ = cal;
    if (apply && !apply_power())
    {
        cal_ = previous;
        apply_power();
        return false;
    }
    return true;
}

bool wifi_tx::applied_dbm(int8_t* out) const
{
    const int8_t v = applied_dbm_.load(std::memory_order_relaxed);
    if (v == INT8_MIN || out == nullptr)
    {
        return false;
    }
    *out = v;
    return true;
}

bool wifi_tx::apply_cca()
{
    const int err = hal_mac_tx_set_cca(cca_enabled_ ? 1 : 0);
    if (err != 0)
    {
        ESP_LOGE(TAG, "hal_mac_tx_set_cca failed: %d", err);
        return false;
    }

    if (cca_enabled_)
    {
        if (phy_cca_off && phy_enable_cca)
        {
            phy_enable_cca();
            phy_cca_off = false;
        }
        return true;
    }

    if (esp_rom_phy_disable_cca)
    {
        esp_rom_phy_disable_cca();
        phy_cca_off = true;
    }
    else if (phy_disable_cca)
    {
        phy_disable_cca();
        phy_cca_off = true;
    }
    return true;
}

bool wifi_tx::init(uint8_t capacity)
{
    if (!q.init(capacity))
    {
        ESP_LOGE(TAG, "tx queue init capacity=%u failed (max %u)",
                 static_cast<unsigned>(capacity),
                 static_cast<unsigned>(k_queue_max));
        return false;
    }
    last_progress_us_.store(esp_timer_get_time(), std::memory_order_relaxed);
    return true;
}

uint8_t wifi_tx::in_flight() const
{
    return static_cast<uint8_t>(in_flight_.load(std::memory_order_relaxed));
}

uint32_t wifi_tx::dropped_invalid_frame() const
{
    return dropped_invalid_frame_.load(std::memory_order_relaxed);
}

uint32_t wifi_tx::dropped_tx_queue() const
{
    return dropped_tx_queue_.load(std::memory_order_relaxed);
}

uint32_t wifi_tx::dropped_wifi() const
{
    return dropped_wifi_.load(std::memory_order_relaxed);
}

uint32_t wifi_tx::air_pkt() const
{
    return air_pkt_.load(std::memory_order_relaxed);
}

void wifi_tx::note_invalid_frame()
{
    dropped_invalid_frame_.fetch_add(1, std::memory_order_relaxed);
}

static void delay_us(uint32_t us)
{
    if (us == 0)
    {
        return;
    }
    if (us >= 1000u)
    {
        vTaskDelay(pdMS_TO_TICKS((us + 999u) / 1000u));
        return;
    }
    esp_rom_delay_us(us);
}

bool wifi_tx::enqueue(packet&& pkt)
{
    if (!q.ready() || !pkt.is_valid())
    {
        pkt.reset();
        return false;
    }
    std::optional<packet> item(std::move(pkt));
    if (q.try_push(std::move(item)))
    {
        return true;
    }
    dropped_tx_queue_.fetch_add(1, std::memory_order_relaxed);
    if (item.has_value())
    {
        item->reset();
    }
    return false;
}

uint8_t wifi_tx::queue_size() const
{
    return q.size();
}

bool wifi_tx::queue_full() const
{
    return !q.ready() || q.size() >= q.limit();
}

void wifi_tx::reset_in_flight()
{
    const uint32_t prev = in_flight_.exchange(0, std::memory_order_relaxed);
    if (prev != 0)
    {
        dropped_wifi_.fetch_add(prev, std::memory_order_relaxed);
        ESP_LOGW(TAG, "reset in_flight from %u", static_cast<unsigned>(prev));
    }
    last_progress_us_.store(esp_timer_get_time(), std::memory_order_relaxed);
}

uint32_t wifi_tx::submits() const
{
    return submits_.load(std::memory_order_relaxed);
}

uint32_t wifi_tx::nomem() const
{
    return nomem_.load(std::memory_order_relaxed);
}

uint32_t wifi_tx::slot_waits() const
{
    return slot_waits_.load(std::memory_order_relaxed);
}

void wifi_tx::wait_for_driver_slot()
{
    if (in_flight_.load(std::memory_order_relaxed) >= WIFI_RADIO_MAX_IN_FLIGHT)
    {
        slot_waits_.fetch_add(1, std::memory_order_relaxed);
    }
    // Bound outstanding TX so we do not storm NO_MEM and starve EMAC DMA.
    for (int spin = 0; in_flight_.load(std::memory_order_relaxed) >=
                       WIFI_RADIO_MAX_IN_FLIGHT;
         ++spin)
    {
        const int64_t now = esp_timer_get_time();
        const int64_t last =
            last_progress_us_.load(std::memory_order_relaxed);
        if (last > 0 && now - last > WIFI_TX_STALL_US)
        {
            reset_in_flight();
            break;
        }
        if (spin < WIFI_RADIO_INJECT_NOMEM_YIELD)
        {
            taskYIELD();
        }
        else
        {
            vTaskDelay(1);
            spin = 0;
        }
    }
}

bool wifi_tx::release_driver_slot()
{
    // Clamped: TX-done also fires for driver-originated frames.
    uint32_t n = in_flight_.load(std::memory_order_relaxed);
    while (n > 0 && !in_flight_.compare_exchange_weak(
                        n, n - 1, std::memory_order_relaxed))
    {
    }
    last_progress_us_.store(esp_timer_get_time(), std::memory_order_relaxed);
    return n > 0;
}

void wifi_tx::note_inject_fail(esp_err_t err)
{
    dropped_wifi_.fetch_add(1, std::memory_order_relaxed);
    ++fail_count_;
    const int64_t now = esp_timer_get_time();
    if (now - fail_log_us_ < k_fail_log_interval_us)
    {
        return;
    }
    fail_log_us_ = now;
    ESP_LOGW(TAG, "80211_tx dropped frame: %s (%u drops since last log)",
             esp_err_to_name(err), static_cast<unsigned>(fail_count_));
    fail_count_ = 0;
}

bool wifi_tx::inject_retry(const uint8_t* frame, size_t len)
{
    int fail_tries = 0;
    int nomem_tries = 0;
    int64_t nomem_start_us = 0;
    for (;;)
    {
        // Do not hold radio.lock across 80211_tx: that call posts into the
        // Wi-Fi task (same core as TX-done / promiscuous). A lock inversion
        // there stalls completions, so the driver ring never drains.
        wait_for_driver_slot();
        // Count before submitting: TX-done can run before 80211_tx returns.
        in_flight_.fetch_add(1, std::memory_order_relaxed);
        submits_.fetch_add(1, std::memory_order_relaxed);
        const esp_err_t err = esp_wifi_80211_tx(
            WIFI_IF_STA, frame, static_cast<int>(len), false);
        if (err == ESP_OK)
        {
            last_progress_us_.store(esp_timer_get_time(),
                                    std::memory_order_relaxed);
            return true;
        }
        release_driver_slot();
        if (err == ESP_ERR_NO_MEM)
        {
            nomem_.fetch_add(1, std::memory_order_relaxed);
            const int64_t now = esp_timer_get_time();
            if (nomem_start_us == 0)
            {
                nomem_start_us = now;
            }
            else if (now - nomem_start_us > WIFI_RADIO_INJECT_NOMEM_MAX_US)
            {
                note_inject_fail(err);
                return false;
            }
            // Prefer yield over 1 ms sleep: TX-done on this core often frees
            // the driver ring within tens of µs.
            if (++nomem_tries <= WIFI_RADIO_INJECT_NOMEM_YIELD)
            {
                taskYIELD();
            }
            else
            {
                vTaskDelay(1);
            }
            continue;
        }
        if (++fail_tries >= WIFI_RADIO_INJECT_RETRIES)
        {
            note_inject_fail(err);
            return false;
        }
        if (fail_tries <= 2)
        {
            taskYIELD();
        }
        else
        {
            vTaskDelay(1);
        }
    }
}

void wifi_tx::run()
{
    for (;;)
    {
        std::optional<packet> slot;
        if (!q.pop(&slot, portMAX_DELAY) || !slot.has_value())
        {
            continue;
        }
        const packet& out = *slot;
        if (out.data() == nullptr || out.size() < WIFI_RADIO_INJECT_MIN ||
            out.size() > WIFI_RADIO_INJECT_MAX)
        {
            note_invalid_frame();
            continue;
        }

        if (inject_retry(out.data(), out.size()))
        {
            radio.pulse_tx_led();
        }

        burst_sent_++;
        if (burst_sent_ >= WIFI_TX_BURST_SIZE_DEFAULT)
        {
            delay_us(WIFI_TX_BURST_GAP_US);
            burst_sent_ = 0;
        }
        else if (queue_size() == 0)
        {
            burst_sent_ = 0;
        }
        else
        {
            taskYIELD();
        }
    }
}

void wifi_tx::task(void* arg)
{
    static_cast<wifi_tx*>(arg)->run();
}

bool wifi_tx::start(BaseType_t core, UBaseType_t prio, uint32_t stack_bytes)
{
    if (!q.ready())
    {
        ESP_LOGE(TAG, "wifi_tx start without queue");
        return false;
    }
    if (xTaskCreatePinnedToCore(task, "wifi_tx", stack_bytes, this, prio,
                                &task_handle_, core) != pdPASS)
    {
        ESP_LOGE(TAG, "wifi_tx task create failed");
        return false;
    }
    return true;
}

bool wifi_tx::set_cca_enabled(bool enabled)
{
    if (cca_enabled_ == enabled)
    {
        return true;
    }
    const bool previous = cca_enabled_;
    cca_enabled_ = enabled;
    if (!apply_cca())
    {
        cca_enabled_ = previous;
        return false;
    }
    return true;
}

bool wifi_tx::set_tx_power(int8_t dbm)
{
    if (dbm < WIFI_TX_POWER_DBM_MIN || dbm > WIFI_TX_POWER_DBM_MAX)
    {
        return false;
    }
    if (tx_power_dbm == dbm)
    {
        return true;
    }
    const int8_t previous = tx_power_dbm;
    tx_power_dbm = dbm;
    if (!apply_power())
    {
        tx_power_dbm = previous;
        apply_power();
        return false;
    }
    return true;
}

void wifi_tx::on_tx_done(const esp_80211_tx_info_t* info)
{
    wifi_tx& tx = wifi::instance().tx();
    if (!tx.release_driver_slot())
    {
        return;
    }
    if (info != nullptr && info->tx_status == WIFI_SEND_SUCCESS)
    {
        tx.air_pkt_.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        tx.dropped_wifi_.fetch_add(1, std::memory_order_relaxed);
    }
}

bool wifi_tx::apply_tx_done_cb()
{
    const esp_err_t err = esp_wifi_register_80211_tx_cb(on_tx_done);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "register_80211_tx_cb failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}
