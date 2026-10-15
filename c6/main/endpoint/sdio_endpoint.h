#ifndef WINJECT_SDIO_ENDPOINT_H_
#define WINJECT_SDIO_ENDPOINT_H_

#include "config.h"

#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

class wifi_rx;
class wifi_tx;

// D-plane over the SDIO link (docs/sdio-link.md). Payloads are the Ethernet
// radio's UDP 9000 datagrams:
//   P4 -> C6: 1-23 bytes register the peer (the P4); 24-1472 bytes are MPDUs
//             to inject (copied and queued on wifi_tx).
//   C6 -> P4: each received frame (MPDU + 4-byte trailer) from wifi_rx.
// Counters keep the Ethernet radio's meaning: ether_pkt is "crossed the host
// link", dropped_send_failed is "no SDIO send buffer in time".
class sdio_endpoint
{
public:
    static sdio_endpoint& instance();
    sdio_endpoint(const sdio_endpoint&) = delete;
    sdio_endpoint& operator=(const sdio_endpoint&) = delete;

    // Link receive task: one d-plane message from the P4.
    void on_dplane(const uint8_t* payload, uint16_t len);

    // Inject frames are dropped until a sink is set (radio down, OTA mode).
    void set_sink(wifi_tx& tx);
    // Starts the forward task (once).
    bool start_forward(wifi_rx& rx, UBaseType_t prio = UPSTREAM_RX_TASK_PRIO,
                       uint32_t stack_bytes = UPSTREAM_RX_TASK_STACK);

    uint32_t tx_ether_pkt() const;
    uint32_t rx_ether_pkt() const;
    uint32_t rx_dropped_no_peer() const;
    uint32_t rx_dropped_send_failed() const;

private:
    sdio_endpoint() = default;
    static void forward_task(void* arg);
    void run_forward();

    std::atomic<wifi_tx*> tx_{nullptr};
    wifi_rx* rx_ = nullptr;
    TaskHandle_t forward_task_ = nullptr;
    std::atomic<bool> peer_{false};
    std::atomic<uint32_t> tx_ether_pkt_{0};
    std::atomic<uint32_t> rx_ether_pkt_{0};
    std::atomic<uint32_t> rx_dropped_no_peer_{0};
    std::atomic<uint32_t> rx_dropped_send_failed_{0};
};

#endif  // WINJECT_SDIO_ENDPOINT_H_
