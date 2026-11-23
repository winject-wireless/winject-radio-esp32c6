#ifndef WINJECT_MPLANE_MPLANE_BACKEND_H_
#define WINJECT_MPLANE_MPLANE_BACKEND_H_

#include "config_types.h"
#include "frame.h"

#include <array>
#include <optional>
#include <stdint.h>

// Device-side operations behind the m-plane commands. The firmware and the
// host simulator each implement these; mplane_commands owns parsing and reply
// formatting. Calls arrive on the m-plane task and must not block for long.

enum class mplane_status : uint8_t
{
    ok,
    invalid,      // EINVAL
    already,      // EALREADY: a test is in progress
    stale,        // ESTALE: request id is stale / does not match
    no_device,    // ENODEV: radio down or OTA mode
    io_error,     // EIO: NVS / driver failure
    not_found,    // ENOENT: empty settings slot
    unsupported,  // ENOTSUP
};

// POSIX-style token used in `NOK <code>` replies.
const char* mplane_status_code(mplane_status status);

using mac_address = std::array<uint8_t, 6>;

// Unset fields keep their current value.
struct radio_patch
{
    std::optional<uint8_t> channel;
    std::optional<int8_t> tx_power_dbm;
    const char* modulation = nullptr;
    std::optional<bool> cca_enabled;
};

// Per-board TX power calibration: one curve per rate group, mapping the
// normalized tx_power (dBm) to the radio's own power control value ("drv").
// Points are joined by straight lines; see the radio's tx_power_map.h.
constexpr size_t k_radio_tx_cal_points_max = 12;

struct radio_tx_cal_point
{
    uint8_t drv = 0;
    int16_t dbm_x100 = 0;
};

struct radio_tx_cal_curve
{
    const char* group = nullptr;  // e.g. "dsss", "ofdm"
    uint8_t n = 0;
    radio_tx_cal_point p[k_radio_tx_cal_points_max];
    bool is_default = true;       // built-in curve (nothing stored for it)
};

// reset without group: every group back to its built-in curve. reset with
// group: that group. Otherwise group's curve is replaced by `curve`.
struct radio_tx_cal_patch
{
    const char* group = nullptr;
    bool reset = false;
    radio_tx_cal_curve curve;
};

struct rx_test_stats
{
    uint64_t pkt = 0;
    uint64_t byt = 0;
    uint64_t fec_error_pkt = 0;
};

enum class fcs_mode : uint8_t
{
    signal,
    actual,
};

struct radio_caps
{
    fcs_mode fcs = fcs_mode::actual;
};

// count == 0 stops the run identified by id. rate_kbps == 0 = unpaced.
struct ether_tx_request
{
    uint8_t id = 0;
    uint32_t host = 0;
    uint16_t port = 0;
    uint16_t mtu = 0;
    uint16_t count = 0;
    uint32_t rate_kbps = 0;
};

// All filters disabled = leave test mode.
struct wifi_rx_match
{
    mac_filter addr1;
    mac_filter addr2;
    mac_filter addr3;

    bool active() const
    {
        return addr1.enabled || addr2.enabled || addr3.enabled;
    }
};

// mtu = full MPDU length (24-byte header + payload). Unset addresses default
// to broadcast (addr1), the radio's own MAC (addr2), and zero (addr3).
// count == 0 stops the run.
struct wifi_tx_request
{
    std::optional<uint8_t> id;
    std::optional<mac_address> addr1;
    std::optional<mac_address> addr2;
    std::optional<mac_address> addr3;
    uint16_t mtu = 0;
    uint16_t count = 0;
    uint32_t rate_kbps = 0;
};

class mplane_device_backend
{
public:
    virtual ~mplane_device_backend() = default;

    // Persists `mode` when given, then restarts after the reply is flushed.
    virtual mplane_status restart(std::optional<WinjectMode> mode) = 0;

    virtual network_config network() const = 0;
    virtual mplane_status set_network(const network_config& cfg) = 0;

    // Configured (not yet booted) values; see tune_config.
    virtual tune_config tune() const = 0;
    virtual mplane_status set_tune(const tune_config& cfg) = 0;

    virtual mplane_status save(uint8_t slot) = 0;
    virtual mplane_status load(uint8_t slot) = 0;

    virtual int64_t uptime_us() const = 0;

    // Full component version, e.g. "v1.0.0".
    virtual const char* version() const = 0;
};

class mplane_radio_backend
{
public:
    virtual ~mplane_radio_backend() = default;

    // Current occupancy, not configured capacity.
    virtual uint8_t tx_queue_size() const = 0;
    virtual uint8_t tx_in_flight() const = 0;
    virtual uint8_t rx_queue_size() const = 0;

    virtual uint32_t tx_dropped_invalid_frame() const = 0;
    virtual uint32_t tx_dropped_tx_queue() const = 0;
    virtual uint32_t tx_dropped_wifi() const = 0;
    virtual uint32_t rx_dropped_filter_mismatched() const = 0;
    virtual uint32_t rx_dropped_rx_queue() const = 0;
    virtual uint32_t rx_dropped_no_peer() const = 0;
    virtual uint32_t rx_dropped_send_failed() const = 0;
    virtual uint32_t tx_ether_pkt() const = 0;
    virtual uint32_t rx_ether_pkt() const = 0;
    virtual uint32_t tx_air_pkt() const = 0;
    virtual uint32_t rx_air_pkt() const = 0;

    virtual radio_config radio() const = 0;
    virtual radio_caps caps() const = 0;
    // false until a frame has been received.
    virtual bool rx_rssi(int8_t* dbm) const = 0;
    // TX power in force after the last apply, normalized dBm (the radio's own
    // read-back; may differ from the request when it is out of range). false
    // when the radio cannot read it back (the radio_tx line then omits it).
    virtual bool tx_power_applied_dbm(int8_t* dbm) const
    {
        (void)dbm;
        return false;
    }
    virtual mplane_status set_radio(const radio_patch& patch) = 0;
    // Chip temperature in 0.1 degC (diagnostics; radio_tx_info prints it as
    // `radio_temp c=<degC>`). false when the radio has no sensor.
    virtual bool temperature_c_x10(int16_t* out) const
    {
        (void)out;
        return false;
    }
    // TX power calibration groups (0 = not calibratable); set applies it at
    // once and stores it on the board.
    virtual size_t tx_cal_groups() const
    {
        return 0;
    }
    virtual mplane_status tx_cal(size_t index, radio_tx_cal_curve* out) const
    {
        (void)index;
        (void)out;
        return mplane_status::unsupported;
    }
    virtual mplane_status set_tx_cal(const radio_tx_cal_patch& patch)
    {
        (void)patch;
        return mplane_status::unsupported;
    }

    virtual mac_filter rx_filter_addr3() const = 0;
    virtual mplane_status set_rx_filter_addr3(const mac_filter& filter) = 0;

    // Space-separated modulation names for `help`.
    virtual const char* modulation_list() const = 0;
};

// test_sdio: SDIO link bandwidth test against the P4 proxy (docs/sdio-link.md,
// "Test plane").
enum class sdio_test_dir : uint8_t
{
    p4c6,  // the P4 sends, the C6 counts
    c6p4,  // the C6 sends, the P4 counts
    both,
};

enum class sdio_test_probe : uint8_t
{
    off,
    internal,  // internal SRAM on both chips
    psram,     // P4 PSRAM (the C6 has none; its probe stays off)
};

struct sdio_test_request
{
    uint32_t count = 0;  // packets per direction; 0 = stop a running test
    uint16_t mtu = 0;
    uint32_t pacing_us = 0;  // 0 = as fast as the link takes them
    sdio_test_dir dir = sdio_test_dir::both;
    sdio_test_probe probe = sdio_test_probe::off;
    uint32_t base_ms = 500;  // idle baseline window before the traffic; 0 = none
    // P4 AXI performance monitor ports for its two units (sdio_test.h
    // SDIO_TEST_AXI_*: 1 cache ... 7 psram ... 9 lcd; 0 = unit off). Measured
    // only on a P4 that has the monitor (rev 3.0+); elsewhere reported as
    // unavailable.
    uint8_t axi_agent[2] = {1, 7};
};

// One direction: sender and receiver counts.
struct sdio_test_flow
{
    bool ran = false;
    uint32_t sent = 0;
    uint32_t send_fail = 0;
    uint32_t sender_us = 0;
    uint32_t rx = 0;
    uint32_t lost = 0;
    uint32_t reorder = 0;
    uint32_t bad = 0;
    uint32_t rx_us = 0;  // first packet received to last
    uint64_t rx_bytes = 0;
};

// CPU and memory probe on one chip over one window.
struct sdio_test_window_stats
{
    bool valid = false;
    uint32_t window_us = 0;
    uint8_t cores = 0;
    uint16_t busy_permille[2] = {0, 0};  // 0xffff = unknown
    bool probe_ok = false;
    uint64_t probe_bytes = 0;
    uint32_t probe_us = 0;
    // AXI monitor (P4 rev 3.0+): port per unit (0 = not measured) and its
    // data-channel use in 1/1000 of the bus cycles, mean and busiest window.
    uint8_t axi_agent[2] = {0, 0};
    uint16_t axi_rd_mean_permille[2] = {0, 0};
    uint16_t axi_wr_mean_permille[2] = {0, 0};
    uint16_t axi_rd_max_permille[2] = {0, 0};
    uint16_t axi_wr_max_permille[2] = {0, 0};
    uint32_t axi_samples = 0;
};

enum class sdio_test_state : uint8_t
{
    idle,
    running,
    done,
    stopped,
    failed,
};

struct sdio_test_result
{
    sdio_test_state state = sdio_test_state::idle;
    const char* error = nullptr;  // with failed: short token
    uint16_t run = 0;
    sdio_test_request req;
    sdio_test_flow p4c6;
    sdio_test_flow c6p4;
    sdio_test_window_stats c6_base, c6_load, p4_base, p4_load;
    uint32_t p4_credit_waits = 0;  // P4 sends that found no free C6 buffer (load window)
    uint32_t p4_rx_reads = 0;      // P4 host reads (load window)
    uint32_t p4_tx_writes = 0;     // P4 host writes to the C6 (load window)
    uint32_t p4_tx_slots = 0;      // C6 receive buffers they filled
    uint32_t p4_tx_msgs = 0;       // messages they carried
    uint32_t p4_link_freq_khz = 0; // SDIO clock (0 = P4 too old to say)
    uint32_t p4_link_downs = 0;    // link losses in the load window
};

class mplane_test_backend
{
public:
    virtual ~mplane_test_backend() = default;

    // Starts the test in the background (already: one is running), or stops
    // it with count 0.
    virtual mplane_status sdio_test(const sdio_test_request& req)
    {
        (void)req;
        return mplane_status::unsupported;
    }
    virtual mplane_status sdio_test_stat(sdio_test_result* out) const
    {
        (void)out;
        return mplane_status::unsupported;
    }

    // port == 0 closes the test socket.
    virtual mplane_status set_ether_rx_port(uint16_t port) = 0;
    virtual rx_test_stats ether_rx_stats(bool clear) = 0;
    virtual mplane_status ether_tx(const ether_tx_request& req) = 0;

    virtual mplane_status set_wifi_rx_match(const wifi_rx_match& match) = 0;
    virtual rx_test_stats wifi_rx_stats(bool clear) = 0;
    virtual mplane_status wifi_tx(const wifi_tx_request& req) = 0;
};

#endif  // WINJECT_MPLANE_MPLANE_BACKEND_H_
