#ifndef WINJECT_DIAG_SDIO_BWTEST_H_
#define WINJECT_DIAG_SDIO_BWTEST_H_

#include "mplane_backend.h"
#include "sdio_test.h"

#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// test_sdio: the C6 runs the SDIO bandwidth test against the P4 proxy over the
// test plane (sdio_test.h). One run in the background:
//   1. P4 window opened with the probe and AXI monitor (get_rx_stats clear),
//      C6 probe started
//   2. base_ms with no test traffic: CPU / probe baseline on both chips
//   3. traffic: bw_tx to the P4 (p4c6) and / or the C6's own sender (c6p4)
//   4. load window read on both chips (P4: get_rx_stats, which also carries
//      its count of the C6's packets), probes off
class sdio_bwtest
{
public:
    static sdio_bwtest& instance();
    sdio_bwtest(const sdio_bwtest&) = delete;
    sdio_bwtest& operator=(const sdio_bwtest&) = delete;

    // Link receive task: one test-plane message from the P4.
    void on_message(const uint8_t* payload, uint16_t len);

    mplane_status start(const sdio_test_request& req);
    sdio_test_result result() const;

private:
    sdio_bwtest();
    static void task(void* arg);
    void run();
    // Sends msg and waits for the reply armed as `key` (arm()). A reply
    // shorter than out_len but at least min_len (0 = out_len) is accepted,
    // the rest of out zeroed.
    bool request(const void* msg, size_t len, uint32_t key, void* out, size_t out_len,
                 uint32_t timeout_ms, size_t min_len = 0);
    bool send(const void* msg, size_t len);
    // Reads the P4's window; with SDIO_TEST_FLAG_CLEAR opens the next one with
    // `probe` and the AXI ports `axi` (2 entries; nullptr = monitor off).
    bool get_p4_window(uint8_t flags, uint8_t probe, const uint8_t* axi,
                       sdio_test_rx_stats_t* out);
    void generate(sdio_test_flow* flow);
    bool send_bw_tx_stop();
    bool wait_bw_tx_done(uint32_t key, sdio_test_bw_tx_done_t* done, const char** error);
    void finish(sdio_test_state state, const char* error);

    sdio_test_request req_;
    uint16_t run_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};

    // Reply mailbox: one outstanding request at a time. expect_ = the awaited
    // type | tag << 8 (0 = none); the receive task claims it by exchanging it
    // for 0, then fills reply_ and stamps reply_key_ before giving reply_sem_,
    // so a waiter only takes a reply stamped with its own key (a late one for
    // a timed-out request is skipped). rx_stats replies also echo the tag.
    uint32_t arm(uint8_t type);
    bool wait_reply(uint32_t key, uint32_t timeout_ms);
    SemaphoreHandle_t reply_sem_ = nullptr;
    std::atomic<uint32_t> expect_{0};
    uint8_t tag_ = 0;
    uint32_t reply_key_ = 0;
    uint8_t reply_[sizeof(sdio_test_rx_stats_t)]{};
    size_t reply_len_ = 0;

    // P4 -> C6 test packets (link receive task writes, runner reads).
    mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
    sdio_test_rx_t rx_{};

    mutable SemaphoreHandle_t result_mu_ = nullptr;
    sdio_test_result result_;
};

#endif  // WINJECT_DIAG_SDIO_BWTEST_H_
