#include "test_backend_esp.h"

#include "sdio_bwtest.h"
#include "wifi.h"

test_backend_esp::test_backend_esp(wifi& radio) : radio_(radio), wifi_tx_(radio)
{
}

mplane_status test_backend_esp::set_ether_rx_port(uint16_t port)
{
    (void)port;
    return mplane_status::unsupported;
}

rx_test_stats test_backend_esp::ether_rx_stats(bool clear)
{
    (void)clear;
    return rx_test_stats{};
}

mplane_status test_backend_esp::ether_tx(const ether_tx_request& req)
{
    (void)req;
    return mplane_status::unsupported;
}

mplane_status test_backend_esp::set_wifi_rx_match(const wifi_rx_match& match)
{
    if (!radio_.ready())
    {
        return mplane_status::no_device;
    }
    radio_.rx().set_test_match(match.addr1, match.addr2, match.addr3);
    return mplane_status::ok;
}

rx_test_stats test_backend_esp::wifi_rx_stats(bool clear)
{
    const wifi_rx::test_counters c = radio_.rx().test_stats(clear);
    rx_test_stats out;
    out.pkt = c.pkt;
    out.byt = c.byt;
    out.fec_error_pkt = c.fcs_error_pkt;
    return out;
}

mplane_status test_backend_esp::wifi_tx(const wifi_tx_request& req)
{
    return wifi_tx_.run(req);
}

mplane_status test_backend_esp::sdio_test(const sdio_test_request& req)
{
    return sdio_bwtest::instance().start(req);
}

mplane_status test_backend_esp::sdio_test_stat(sdio_test_result* out) const
{
    *out = sdio_bwtest::instance().result();
    return mplane_status::ok;
}
