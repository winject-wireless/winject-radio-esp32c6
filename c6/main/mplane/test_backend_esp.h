#ifndef WINJECT_MPLANE_TEST_BACKEND_ESP_H_
#define WINJECT_MPLANE_TEST_BACKEND_ESP_H_

#include "mplane_backend.h"
#include "wifi_tx_test.h"

class wifi;

// mplane_test_backend: WiFi test tap/generator. The Ethernet tests reply
// ENOTSUP (no IP stack on the C6). WiFi tests reply ENODEV while the radio is
// down.
class test_backend_esp : public mplane_test_backend
{
public:
    explicit test_backend_esp(wifi& radio);

    mplane_status set_ether_rx_port(uint16_t port) override;
    rx_test_stats ether_rx_stats(bool clear) override;
    mplane_status ether_tx(const ether_tx_request& req) override;

    mplane_status set_wifi_rx_match(const wifi_rx_match& match) override;
    rx_test_stats wifi_rx_stats(bool clear) override;
    mplane_status wifi_tx(const wifi_tx_request& req) override;

private:
    wifi& radio_;
    wifi_tx_test wifi_tx_;
};

#endif  // WINJECT_MPLANE_TEST_BACKEND_ESP_H_
