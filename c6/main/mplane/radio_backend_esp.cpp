#include "radio_backend_esp.h"

#include "sdio_endpoint.h"
#include "settings.h"
#include "wifi.h"

#include "esp_log.h"

#include <string.h>

static const char* TAG = "radio_mplane";

radio_backend_esp::radio_backend_esp(wifi& radio) : radio_(radio) {}

uint8_t radio_backend_esp::tx_queue_size() const
{
    return radio_.tx().queue_size();
}

uint8_t radio_backend_esp::tx_in_flight() const
{
    return radio_.tx().in_flight();
}

uint8_t radio_backend_esp::rx_queue_size() const
{
    return radio_.rx().queue_size();
}

uint32_t radio_backend_esp::tx_dropped_invalid_frame() const
{
    return radio_.tx().dropped_invalid_frame();
}

uint32_t radio_backend_esp::tx_dropped_tx_queue() const
{
    return radio_.tx().dropped_tx_queue();
}

uint32_t radio_backend_esp::tx_dropped_wifi() const
{
    return radio_.tx().dropped_wifi();
}

uint32_t radio_backend_esp::rx_dropped_filter_mismatched() const
{
    return radio_.rx().dropped_filter_mismatched();
}

uint32_t radio_backend_esp::rx_dropped_rx_queue() const
{
    return radio_.rx().dropped_rx_queue();
}

uint32_t radio_backend_esp::rx_dropped_no_peer() const
{
    return sdio_endpoint::instance().rx_dropped_no_peer();
}

uint32_t radio_backend_esp::rx_dropped_send_failed() const
{
    return sdio_endpoint::instance().rx_dropped_send_failed();
}

uint32_t radio_backend_esp::tx_ether_pkt() const
{
    return sdio_endpoint::instance().tx_ether_pkt();
}

uint32_t radio_backend_esp::rx_ether_pkt() const
{
    return sdio_endpoint::instance().rx_ether_pkt();
}

uint32_t radio_backend_esp::tx_air_pkt() const
{
    return radio_.tx().air_pkt();
}

uint32_t radio_backend_esp::rx_air_pkt() const
{
    return radio_.rx().air_pkt();
}

radio_config radio_backend_esp::radio() const
{
    radio_config out{};
    if (!radio_.config(&out))
    {
        ESP_LOGW(TAG, "radio(): config lock failed");
    }
    return out;
}

radio_caps radio_backend_esp::caps() const
{
    return radio_caps{fcs_mode::signal};
}

bool radio_backend_esp::rx_rssi(int8_t* dbm) const
{
    return radio_.rx().last_rssi(dbm);
}

bool radio_backend_esp::tx_power_applied_dbm(int8_t* dbm) const
{
    return radio_.tx().applied_dbm(dbm);
}

mplane_status radio_backend_esp::set_radio(const radio_patch& patch)
{
    if (!radio_.ready())
    {
        return mplane_status::no_device;
    }
    radio_config cur{};
    if (!radio_.config(&cur))
    {
        return mplane_status::io_error;
    }
    const uint8_t channel = patch.channel.value_or(cur.channel);
    const char* modulation =
        patch.modulation != nullptr ? patch.modulation : cur.modulation;
    if (!wifi::modulation_supported(modulation, channel))
    {
        return mplane_status::invalid;
    }

    // Channel 14 is 802.11b-only: switch to DSSS/CCK before entering it, and
    // leave it before switching to OFDM.
    bool ok = true;
    if (channel == 14)
    {
        ok = (patch.modulation == nullptr || radio_.set_modulation(modulation)) &&
             (!patch.channel || radio_.set_channel(channel));
        if (!ok)
        {
            if (patch.modulation != nullptr)
            {
                radio_.set_modulation(cur.modulation);
            }
            if (patch.channel)
            {
                radio_.set_channel(cur.channel);
            }
        }
    }
    else
    {
        ok = (!patch.channel || radio_.set_channel(channel)) &&
             (patch.modulation == nullptr || radio_.set_modulation(modulation));
        if (!ok)
        {
            if (patch.modulation != nullptr)
            {
                radio_.set_modulation(cur.modulation);
            }
            if (patch.channel)
            {
                radio_.set_channel(cur.channel);
            }
        }
    }
    if (ok && patch.cca_enabled)
    {
        ok = radio_.set_cca_enabled(*patch.cca_enabled);
    }
    if (ok && patch.tx_power_dbm)
    {
        ok = radio_.set_tx_power(*patch.tx_power_dbm);
    }
    if (!ok)
    {
        ESP_LOGE(TAG, "radio_tx apply failed (channel=%u modulation=%s)",
                 static_cast<unsigned>(channel), modulation);
        return mplane_status::io_error;
    }
    return mplane_status::ok;
}

mac_filter radio_backend_esp::rx_filter_addr3() const
{
    return radio_.rx().filter_addr3();
}

mplane_status radio_backend_esp::set_rx_filter_addr3(const mac_filter& filter)
{
    radio_.rx().set_filter_addr3(filter);
    return mplane_status::ok;
}

const char* radio_backend_esp::modulation_list() const
{
    return wifi::modulation_list();
}

size_t radio_backend_esp::tx_cal_groups() const
{
    return tx_power_map::k_classes;
}

mplane_status radio_backend_esp::tx_cal(size_t index, radio_tx_cal_curve* out) const
{
    tx_power_map::cal cal;
    if (out == nullptr || index >= tx_power_map::k_classes)
    {
        return mplane_status::invalid;
    }
    if (!radio_.tx_cal(&cal))
    {
        return mplane_status::io_error;
    }
    const auto cls = static_cast<tx_power_map::rate_class>(index);
    const tx_power_map::curve& c = cal.c[index];
    out->group = tx_power_map::class_name(cls);
    out->n = c.n;
    for (size_t i = 0; i < c.n; i++)
    {
        out->p[i].drv = c.p[i].q;
        out->p[i].dbm_x100 = c.p[i].dbm_x100;
    }
    out->is_default = tx_power_map::curve_is_default(c, cls);
    return mplane_status::ok;
}

mplane_status radio_backend_esp::set_tx_cal(const radio_tx_cal_patch& patch)
{
    tx_power_map::cal cal;
    if (!radio_.tx_cal(&cal))
    {
        return mplane_status::io_error;
    }
    size_t index = tx_power_map::k_classes;
    if (patch.group != nullptr)
    {
        for (size_t i = 0; i < tx_power_map::k_classes; i++)
        {
            if (strcmp(patch.group, tx_power_map::class_name(
                                        static_cast<tx_power_map::rate_class>(i))) == 0)
            {
                index = i;
            }
        }
        if (index == tx_power_map::k_classes)
        {
            return mplane_status::invalid;
        }
    }
    if (patch.reset && patch.group == nullptr)
    {
        cal = tx_power_map::default_cal();
    }
    else if (patch.reset)
    {
        cal.c[index] = tx_power_map::default_curve(static_cast<tx_power_map::rate_class>(index));
    }
    else
    {
        if (patch.group == nullptr || patch.curve.n > tx_power_map::k_max_points)
        {
            return mplane_status::invalid;
        }
        tx_power_map::curve c;
        c.n = patch.curve.n;
        for (size_t i = 0; i < c.n; i++)
        {
            c.p[i].q = patch.curve.p[i].drv;
            c.p[i].dbm_x100 = patch.curve.p[i].dbm_x100;
        }
        if (!tx_power_map::curve_valid(c))
        {
            return mplane_status::invalid;
        }
        cal.c[index] = c;
    }
    if (!radio_.set_tx_cal(cal))
    {
        return mplane_status::io_error;
    }
    settings& store = settings::instance();
    const bool stored = tx_power_map::cal_is_default(cal) ? store.erase_tx_cal()
                                                           : store.save_tx_cal(cal);
    if (!stored)
    {
        ESP_LOGW(TAG, "tx_cal applied but not stored");
        return mplane_status::io_error;
    }
    ESP_LOGI(TAG, "tx_cal %s %s", patch.group != nullptr ? patch.group : "all",
             patch.reset ? "reset" : "set");
    return mplane_status::ok;
}
