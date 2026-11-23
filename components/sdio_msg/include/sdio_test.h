#pragma once

// SDIO bandwidth test (plane SDIO_MSG_PLANE_TEST, docs/sdio-link.md "Test
// plane"). The C6 runs the test (m-plane `test_sdio`); the P4 proxy only
// answers: it sends test packets when asked (bw_tx), counts the ones it
// receives and reports them with its CPU and memory-probe figures
// (get_rx_stats). Byte 0 of every message is its type; all fields are
// little-endian. No IDF dependencies.
//
//   bw_tx         C6 -> P4  P4 sends `count` test packets of `mtu` bytes,
//                           one every pacing_us (0 = as fast as the link
//                           takes them); count 0 stops a running bw_tx
//   bw_tx_done    P4 -> C6  after the last packet (or a stop)
//   test_packet   both      run, seq, filler up to mtu
//   get_rx_stats  C6 -> P4  P4 replies rx_stats for its current window; with
//                           SDIO_TEST_FLAG_CLEAR it then starts a new window
//                           (receive counters, CPU and probe) for `run`, sets
//                           the memory probe to `probe` and the AXI monitor
//                           (P4 rev 3.0+) to `axi_agent`
//   rx_stats      P4 -> C6
//
// The P4 sends bw_tx_done after its last test packet and answers
// get_rx_stats after every test packet queued before it, so on each side
// a reply covers everything sent before it.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SDIO_TEST_BW_TX 0x01u
#define SDIO_TEST_PACKET 0x02u
#define SDIO_TEST_GET_RX_STATS 0x03u
#define SDIO_TEST_REPLY 0x80u
#define SDIO_TEST_BW_TX_DONE (SDIO_TEST_BW_TX | SDIO_TEST_REPLY)
#define SDIO_TEST_RX_STATS (SDIO_TEST_GET_RX_STATS | SDIO_TEST_REPLY)

// get_rx_stats flags
#define SDIO_TEST_FLAG_CLEAR 0x01u

// Memory probe: a memcpy loop at idle priority on every core (see
// sdio_test_meter.h). The figure is bytes copied per second of the probe's
// own CPU time, so it does not depend on how much idle time it got.
#define SDIO_TEST_PROBE_OFF 0u
#define SDIO_TEST_PROBE_INTERNAL 1u  // internal SRAM (P4: uncached L2MEM)
#define SDIO_TEST_PROBE_PSRAM 2u     // P4 only

// AXI performance monitor ports (ESP32-P4 rev 3.0+ only, TRM chapter 24,
// table 24.5-3). The SDMMC host is not one of them: the monitor shows how busy
// the CPU cache and PSRAM ports are while the link runs, not the SDIO DMA.
#define SDIO_TEST_AXI_NONE 0u
#define SDIO_TEST_AXI_CACHE 1u
#define SDIO_TEST_AXI_VDMA1 2u
#define SDIO_TEST_AXI_VDMA2 3u
#define SDIO_TEST_AXI_DMA2D 4u
#define SDIO_TEST_AXI_H264_1 5u
#define SDIO_TEST_AXI_H264_2 6u
#define SDIO_TEST_AXI_PSRAM 7u
#define SDIO_TEST_AXI_CAMERA 8u
#define SDIO_TEST_AXI_LCD 9u
#define SDIO_TEST_AXI_UNITS 2u

// busy_permille when the firmware has no FreeRTOS run-time stats.
#define SDIO_TEST_CPU_UNKNOWN 0xffffu

#define SDIO_TEST_MAX_CORES 2u

typedef struct __attribute__((packed)) {
    uint8_t type;  // SDIO_TEST_BW_TX
    uint8_t reserved;
    uint16_t run;
    uint16_t mtu;  // sizeof(sdio_test_packet_t) .. SDIO_MSG_MAX_PAYLOAD
    uint16_t reserved2;
    uint32_t count;
    uint32_t pacing_us;
} sdio_test_bw_tx_t;

typedef struct __attribute__((packed)) {
    uint8_t type;  // SDIO_TEST_BW_TX_DONE
    uint8_t reserved;
    uint16_t run;
    uint32_t sent;
    uint32_t send_fail;   // no free buffer on the other side in time
    uint32_t elapsed_us;  // first send to last
} sdio_test_bw_tx_done_t;

// Followed by filler up to the packet's mtu.
typedef struct __attribute__((packed)) {
    uint8_t type;  // SDIO_TEST_PACKET
    uint8_t reserved;
    uint16_t run;
    uint32_t seq;  // 0, 1, ... within the run
} sdio_test_packet_t;

typedef struct __attribute__((packed)) {
    uint8_t type;  // SDIO_TEST_GET_RX_STATS
    uint8_t flags;
    uint8_t probe;  // for the new window (with SDIO_TEST_FLAG_CLEAR)
    uint8_t reserved;
    uint16_t run;   // test packets of this run are counted in the new window
    // AXI monitor ports for the new window (SDIO_TEST_AXI_*, NONE = unit off);
    // ignored where there is no monitor.
    uint8_t axi_agent[SDIO_TEST_AXI_UNITS];
} sdio_test_get_rx_stats_t;

// CPU and memory probe over one measurement window.
typedef struct __attribute__((packed)) {
    uint32_t window_us;
    uint8_t cores;
    uint8_t probe;     // SDIO_TEST_PROBE_* that ran in the window
    uint8_t probe_ok;  // 0: probe off, or its buffers could not be allocated
    uint8_t reserved;
    // Per core: time not spent in the idle task or the probe, in 1/1000 of
    // the window (SDIO_TEST_CPU_UNKNOWN without run-time stats).
    uint16_t busy_permille[SDIO_TEST_MAX_CORES];
    uint64_t probe_bytes;  // copied, all cores
    uint32_t probe_us;     // probe CPU time, all cores
    // AXI monitor, per unit: data beats per bus cycle on the port, in 1/1000.
    // mean: sampled over the window; max: the busiest monitor time window.
    // axi_agent NONE = not measured (no monitor on this chip, or unit off).
    uint8_t axi_agent[SDIO_TEST_AXI_UNITS];
    uint16_t axi_rd_mean_permille[SDIO_TEST_AXI_UNITS];
    uint16_t axi_wr_mean_permille[SDIO_TEST_AXI_UNITS];
    uint16_t axi_rd_max_permille[SDIO_TEST_AXI_UNITS];
    uint16_t axi_wr_max_permille[SDIO_TEST_AXI_UNITS];
    uint32_t axi_samples;
} sdio_test_window_t;

// Test packets of one run, received in one window.
typedef struct __attribute__((packed)) {
    uint32_t pkts;
    uint32_t lost;     // sequence gaps
    uint32_t reorder;  // older than the highest seen (repeats and late ones)
    uint32_t bad;      // shorter than the header
    uint32_t span_us;  // first packet to last
    uint64_t bytes;    // payload bytes (the whole test packet)
} sdio_test_rx_counts_t;

typedef struct __attribute__((packed)) {
    uint8_t type;  // SDIO_TEST_RX_STATS
    uint8_t reserved;
    uint16_t run;
    sdio_test_rx_counts_t rx;
    sdio_test_window_t window;
    // Link host counters over the window.
    uint32_t link_credit_waits;  // sends that found no free C6 buffer
    uint32_t link_rx_reads;      // host reads (each drains several messages)
    uint32_t link_tx_writes;     // host writes to the C6 (each several messages)
    uint32_t link_tx_slots;      // C6 receive buffers those writes filled
    uint32_t link_tx_msgs;       // messages those writes carried
    // Added later: an older P4 sends the reply without these (read as 0).
    uint32_t link_freq_khz;      // SDIO clock now
    uint32_t link_downs;         // link losses over the window (bus errors, C6 resets)
} sdio_test_rx_stats_t;

// Reply length from a P4 that predates link_freq_khz.
#define SDIO_TEST_RX_STATS_MIN_LEN (sizeof(sdio_test_rx_stats_t) - 8)

// Receive counter for one side.
typedef struct {
    uint16_t run;
    bool any;
    uint32_t next_seq;
    int64_t first_us;
    int64_t last_us;
    sdio_test_rx_counts_t c;
} sdio_test_rx_t;

// Starts counting packets of `run` from zero.
void sdio_test_rx_reset(sdio_test_rx_t *rx, uint16_t run);

// One test packet (whole message payload). Packets of another run are
// ignored. now_us: receive time.
void sdio_test_rx_packet(sdio_test_rx_t *rx, const uint8_t *payload, uint16_t len, int64_t now_us);

// Fills a test packet's header; the filler is left as it is.
void sdio_test_packet_set(uint8_t *buf, uint16_t run, uint32_t seq);

#ifdef __cplusplus
}
#endif
