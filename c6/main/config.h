#ifndef WINJECT_CONFIG_H_
#define WINJECT_CONFIG_H_

#include <stdint.h>

#define DEVICE_HOSTNAME "winject-esp32c6"

// No status LEDs: GPIO16/17 (the WT32 LED pins) are the C6's UART0.
#define WIFI_RX_LED_GPIO -1
#define WIFI_TX_LED_GPIO -1
#define WIFI_LED_ON 0
#define WIFI_LED_OFF 1
#define WIFI_LED_STRETCH_US (20 * 1000u)

// Ports of the Ethernet radio. The C6 has no IP stack; the P4 proxy binds these
// and forwards datagrams over SDIO (docs/sdio-link.md). Kept for the m-plane
// checks that reference them.
#define CONTROL_CONSOLE_PORT 22
#define DPLANE_PORT 9000
#define OTA_HTTP_PORT 80
// Largest MPDU in one d-plane datagram. The P4 receives it over UDP on a
// 1500-byte MTU, so the limit is the same as the Ethernet radio's.
#define DPLANE_INJECT_MPDU_MAX 1472

// test_ether_* argument limits (the commands reply ENOTSUP on the C6).
#define ETHER_TEST_MTU_MAX 1472
// Frames between vTaskDelay(1) in the test_wifi_tx loop.
#define ETHER_TEST_YIELD_EVERY 32

// network_config is still stored in settings slots (same blob as the Ethernet
// radio) but not applied: the C6 has no network interface.
#define NETWORK_DHCP_TIMEOUT_S_DEFAULT 5
#define ETH_FALLBACK_ADDR 192, 168, 32, 1
#define NETWORK_PREFIX_DEFAULT 24
// tune eth_* fields: accepted only at the ESP32 build values so settings
// blobs and tune commands stay interchangeable with the Ethernet radio.
#define WINJECT_ETH_RX_RING_FIXED 10
#define WINJECT_ETH_TX_RING_FIXED 10

// One core: everything runs on core 0. Priorities: WiFi driver task (IDF, 23)
// > wifi_tx (20) > SDIO link receive (19) > d-plane forward (17) > m-plane.
#define WIFI_RADIO_TASK_CORE 0
#define APP_TASK_CORE 0
#define WIFI_RADIO_TASK_PRIO 20
#define SDIO_LINK_RX_TASK_PRIO 19
#define UPSTREAM_RX_TASK_PRIO 17
#define UPSTREAM_RX_TASK_STACK 4096
#define MPLANE_TASK_PRIO 5
#define MPLANE_TASK_STACK 6144
// m-plane requests waiting for the m-plane task.
#define MPLANE_QUEUE_LEN 4
// Forwarded frame: wait this long for a free SDIO send buffer, then count it
// as dropped_send_failed.
#define DPLANE_SEND_TIMEOUT_MS 20
// test_wifi_tx generator: below wifi_tx and the d-plane drain.
#define WIFI_TX_TEST_TASK_PRIO 16
#define WIFI_TX_TEST_TASK_STACK 4096

// Raw 802.11 radio.
#define WIFI_RADIO_INJECT_MIN 24
#define WIFI_RADIO_INJECT_MAX 1500
#define WIFI_FCS_LEN 4
// RX pool slot: largest forwarded MPDU plus its on-air FCS.
#define WIFI_RX_PACKET_CAP (WIFI_RADIO_INJECT_MAX + WIFI_FCS_LEN)
// App queues between driver callbacks and worker tasks (tune_tx_param
// tx_queue_sz / tune_rx_param rx_queue_sz, applied at boot). The RX packet
// pool has one WIFI_RX_PACKET_CAP slot per rx_queue_sz, heap-allocated at boot.
#define WIFI_TX_QUEUE_DEFAULT 20
#define WIFI_TX_QUEUE_MAX 64
#define WIFI_RX_QUEUE_DEFAULT 8
#define WIFI_RX_QUEUE_MAX 32
// WiFi driver dynamic buffers (tune wifi_tx_ring_sz / wifi_rx_ring_sz) feed
// wifi_init_config_t at boot. Ranges follow the IDF Kconfig; the dynamic RX
// count must not be below CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM.
#define WIFI_TX_RING_MIN 1
#define WIFI_TX_RING_MAX 128
#define WIFI_RX_RING_MAX 128
// wifi_tx pacing and in-flight cap: the Ethernet radio's values (tuned for EMAC/WiFi
// DMA timesharing on the ESP32), not yet re-tuned for SDIO on the C6.
// wifi_tx: EMAC/WiFi timeshare — drain a small batch then idle (see winject.md).
#define WIFI_TX_BURST_SIZE_DEFAULT 8
#define WIFI_TX_BURST_GAP_US 1000
#define WIFI_RADIO_INJECT_RETRIES 8
// Cap outstanding 802.11 TX before submitting another — avoids NO_MEM storms
// that thrash the WiFi DMA and starve EMAC RX. Do not raise without re-checking
// ETH→sut goodput (see docs/winject.md#ethwifi-tx-performance).
#define WIFI_RADIO_MAX_IN_FLIGHT 4
// NO_MEM: yield this many times before a 1-tick sleep (driver ring recovery).
// Higher helps sustain ~MCS7 30 Mbps inject without thrashing on 1 ms sleeps.
#define WIFI_RADIO_INJECT_NOMEM_YIELD 48
// Total time to retry ESP_ERR_NO_MEM on one frame before dropping it.
#define WIFI_RADIO_INJECT_NOMEM_MAX_US (50 * 1000)
// No TX-done progress for this long → reset in_flight (driver may drop silently).
#define WIFI_TX_STALL_US (100 * 1000)
#define WIFI_CHANNEL_MIN 1
#define WIFI_CHANNEL_MAX 14
#define WIFI_DEFAULT_CHANNEL 1
#define WIFI_DEFAULT_MODULATION "DSS_1M_L"
#define WIFI_DEFAULT_TX_POWER_DBM 20
#define WIFI_TX_POWER_DBM_MIN 2
#define WIFI_TX_POWER_DBM_MAX 20
// Legacy 64-QAM (48M/54M) raw inject clips above this; peer promisc RX fails
// while USB monitor still looks fine at 20 dBm.
#define WIFI_TX_POWER_64QAM_LEGACY_MAX_DBM 13

#define WIFI_HDR_LEN 24
// Winject Addr3 prefix (manager domain encoding); not used as an RX filter
// default — cleared rx_filter_addr3 forwards all frames.
#define WIFI_BSSID_PREFIX_STANDALONE 0xCA, 0xFE, 0xBA, 0xBE

#define SETTINGS_SLOT_COUNT 10
// Longest modulation name + NUL ("OFDM_MCS7_LGI").
#define SETTINGS_MODULATION_MAX 16

#endif  // WINJECT_CONFIG_H_
