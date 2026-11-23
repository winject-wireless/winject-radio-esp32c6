#include "sdio_bwtest.h"

#include "config.h"
#include "sdio_link_slave.h"
#include "sdio_msg.h"
#include "sdio_test_meter.h"
#include "test_pacer.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

static const char* TAG = "sdio_bwtest";

namespace
{
constexpr uint32_t k_reply_timeout_ms = 1000;
// The P4's bw_tx counts as stuck when no test packet arrives for this long.
constexpr uint32_t k_progress_timeout_ms = 3000;
constexpr uint32_t k_send_timeout_ms = 1000;
constexpr uint32_t k_task_stack = 4096;

uint8_t wire_probe(sdio_test_probe p)
{
    switch (p)
    {
    case sdio_test_probe::internal:
        return SDIO_TEST_PROBE_INTERNAL;
    case sdio_test_probe::psram:
        return SDIO_TEST_PROBE_PSRAM;
    default:
        return SDIO_TEST_PROBE_OFF;
    }
}

sdio_test_window_stats to_stats(const sdio_test_window_t& w)
{
    sdio_test_window_stats s;
    s.valid = true;
    s.window_us = w.window_us;
    s.cores = w.cores;
    s.busy_permille[0] = w.busy_permille[0];
    s.busy_permille[1] = w.busy_permille[1];
    s.probe_ok = w.probe_ok != 0;
    s.probe_bytes = w.probe_bytes;
    s.probe_us = w.probe_us;
    for (int u = 0; u < 2; ++u)
    {
        s.axi_agent[u] = w.axi_agent[u];
        s.axi_rd_mean_permille[u] = w.axi_rd_mean_permille[u];
        s.axi_wr_mean_permille[u] = w.axi_wr_mean_permille[u];
        s.axi_rd_max_permille[u] = w.axi_rd_max_permille[u];
        s.axi_wr_max_permille[u] = w.axi_wr_max_permille[u];
    }
    s.axi_samples = w.axi_samples;
    return s;
}

bool sends_p4(sdio_test_dir d)
{
    return d == sdio_test_dir::p4c6 || d == sdio_test_dir::both;
}

bool sends_c6(sdio_test_dir d)
{
    return d == sdio_test_dir::c6p4 || d == sdio_test_dir::both;
}
}  // namespace

sdio_bwtest& sdio_bwtest::instance()
{
    static sdio_bwtest inst;
    return inst;
}

sdio_bwtest::sdio_bwtest()
{
    reply_sem_ = xSemaphoreCreateBinary();
    result_mu_ = xSemaphoreCreateMutex();
}

void sdio_bwtest::on_message(const uint8_t* payload, uint16_t len)
{
    if (len == 0)
    {
        return;
    }
    const uint8_t type = payload[0];
    if (type == SDIO_TEST_PACKET)
    {
        const int64_t now = esp_timer_get_time();
        portENTER_CRITICAL(&mux_);
        sdio_test_rx_packet(&rx_, payload, len, now);
        portEXIT_CRITICAL(&mux_);
        return;
    }
    uint32_t key = expect_.load();
    if (len < 4 || key == 0 || type != (key & 0xffu))
    {
        return;  // unexpected, late (timed out) or malformed reply
    }
    // rx_stats echo the request's tag; 0 = a P4 that predates the tag.
    const uint8_t tag = static_cast<uint8_t>(key >> 8);
    if (type == SDIO_TEST_RX_STATS && payload[1] != 0 && payload[1] != tag)
    {
        return;
    }
    uint16_t run;
    memcpy(&run, payload + 2, sizeof(run));
    if (run != run_)
    {
        return;
    }
    if (!expect_.compare_exchange_strong(key, 0))
    {
        return;  // the waiter gave up meanwhile
    }
    // A newer P4 may append fields: keep what this C6 knows.
    const size_t n = len < sizeof(reply_) ? len : sizeof(reply_);
    memcpy(reply_, payload, n);
    reply_len_ = n;
    reply_key_ = key;
    xSemaphoreGive(reply_sem_);
}

uint32_t sdio_bwtest::arm(uint8_t type)
{
    tag_ = static_cast<uint8_t>(tag_ + 1 == 0 ? 1 : tag_ + 1);
    const uint32_t key = type | (static_cast<uint32_t>(tag_) << 8);
    expect_.store(key);
    return key;
}

bool sdio_bwtest::wait_reply(uint32_t key, uint32_t timeout_ms)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    bool claimed = false;
    for (;;)
    {
        const TickType_t now = xTaskGetTickCount();
        const TickType_t left =
            static_cast<int32_t>(deadline - now) > 0 ? deadline - now : 0;
        if (xSemaphoreTake(reply_sem_, left) == pdTRUE)
        {
            if (reply_key_ == key)
            {
                return true;
            }
            continue;  // a reply to an earlier, timed-out request
        }
        if (claimed)
        {
            return false;  // claimed but never given: not expected
        }
        uint32_t k = key;
        if (expect_.compare_exchange_strong(k, 0))
        {
            return false;
        }
        // The receive task claimed it just now: its give follows shortly.
        claimed = true;
        deadline = xTaskGetTickCount() + pdMS_TO_TICKS(100);
    }
}

mplane_status sdio_bwtest::start(const sdio_test_request& req)
{
    if (req.count == 0)
    {
        if (running_.load())
        {
            stop_.store(true);
        }
        return mplane_status::ok;
    }
    if (req.mtu < sizeof(sdio_test_packet_t) || req.mtu > SDIO_MSG_MAX_PAYLOAD)
    {
        return mplane_status::invalid;
    }
    if (reply_sem_ == nullptr || result_mu_ == nullptr)
    {
        return mplane_status::io_error;
    }
    if (running_.exchange(true))
    {
        return mplane_status::already;
    }
    stop_.store(false);
    req_ = req;
    run_ = static_cast<uint16_t>(run_ + 1 == 0 ? 1 : run_ + 1);
    xSemaphoreTake(result_mu_, portMAX_DELAY);
    result_ = sdio_test_result{};
    result_.state = sdio_test_state::running;
    result_.run = run_;
    result_.req = req;
    xSemaphoreGive(result_mu_);
    if (xTaskCreatePinnedToCore(task, "sdio_bwtest", k_task_stack, this, WIFI_TX_TEST_TASK_PRIO,
                                nullptr, APP_TASK_CORE) != pdPASS)
    {
        finish(sdio_test_state::failed, "no_task");
        running_.store(false);
        return mplane_status::io_error;
    }
    return mplane_status::ok;
}

sdio_test_result sdio_bwtest::result() const
{
    if (result_mu_ == nullptr)
    {
        return sdio_test_result{};
    }
    xSemaphoreTake(result_mu_, portMAX_DELAY);
    const sdio_test_result r = result_;
    xSemaphoreGive(result_mu_);
    return r;
}

void sdio_bwtest::task(void* arg)
{
    auto* self = static_cast<sdio_bwtest*>(arg);
    self->run();
    self->running_.store(false);
    vTaskDelete(nullptr);
}

bool sdio_bwtest::send(const void* msg, size_t len)
{
    return sdio_link_slave_send(SDIO_MSG_PLANE_TEST, msg, static_cast<uint16_t>(len),
                                pdMS_TO_TICKS(k_send_timeout_ms)) == ESP_OK;
}

bool sdio_bwtest::request(const void* msg, size_t len, uint32_t key, void* out, size_t out_len,
                          uint32_t timeout_ms, size_t min_len)
{
    if (min_len == 0 || min_len > out_len)
    {
        min_len = out_len;
    }
    if (!send(msg, len))
    {
        expect_.store(0);
        return false;
    }
    if (!wait_reply(key, timeout_ms))
    {
        return false;
    }
    if (reply_len_ < min_len)
    {
        return false;
    }
    const size_t n = reply_len_ < out_len ? reply_len_ : out_len;
    memset(out, 0, out_len);
    memcpy(out, reply_, n);
    return true;
}

bool sdio_bwtest::get_p4_window(uint8_t flags, uint8_t probe, const uint8_t* axi,
                                sdio_test_rx_stats_t* out)
{
    sdio_test_get_rx_stats_t m{};
    m.type = SDIO_TEST_GET_RX_STATS;
    m.flags = flags;
    m.probe = probe;
    if (axi != nullptr)
    {
        m.axi_agent[0] = axi[0];
        m.axi_agent[1] = axi[1];
    }
    m.run = run_;
    const uint32_t key = arm(SDIO_TEST_RX_STATS);
    m.tag = static_cast<uint8_t>(key >> 8);
    return request(&m, sizeof(m), key, out, sizeof(*out), k_reply_timeout_ms,
                   SDIO_TEST_RX_STATS_MIN_LEN);
}

void sdio_bwtest::generate(sdio_test_flow* flow)
{
    const uint16_t mtu = req_.mtu;
    auto* buf = static_cast<uint8_t*>(malloc(mtu));
    if (buf == nullptr)
    {
        flow->send_fail = req_.count;
        return;
    }
    for (uint16_t i = 0; i < mtu; ++i)
    {
        buf[i] = static_cast<uint8_t>(i * 7 + 1);
    }
    test_pacer pacer(req_.pacing_us);
    const int64_t t0 = esp_timer_get_time();
    for (uint32_t seq = 0; seq < req_.count && !stop_.load(); ++seq)
    {
        pacer.wait();
        sdio_test_packet_set(buf, run_, seq);
        if (send(buf, mtu))
        {
            flow->sent++;
        }
        else
        {
            flow->send_fail++;
        }
    }
    flow->sender_us = static_cast<uint32_t>(esp_timer_get_time() - t0);
    free(buf);
}

bool sdio_bwtest::send_bw_tx_stop()
{
    sdio_test_bw_tx_t m{};
    m.type = SDIO_TEST_BW_TX;
    m.run = run_;
    return send(&m, sizeof(m));  // count 0
}

bool sdio_bwtest::wait_bw_tx_done(uint32_t key, sdio_test_bw_tx_done_t* done,
                                  const char** error)
{
    uint32_t last_pkts = 0;
    uint32_t quiet_ms = 0;
    bool stop_sent = false;
    constexpr uint32_t k_slice_ms = 250;
    for (;;)
    {
        if (xSemaphoreTake(reply_sem_, pdMS_TO_TICKS(k_slice_ms)) == pdTRUE)
        {
            if (reply_key_ != key)
            {
                continue;  // a late reply to an earlier request
            }
            if (reply_len_ < sizeof(*done))
            {
                *error = "bad_reply";
                return false;
            }
            memcpy(done, reply_, sizeof(*done));
            return true;
        }
        if (stop_.load() && !stop_sent)
        {
            stop_sent = send_bw_tx_stop();
        }
        portENTER_CRITICAL(&mux_);
        const uint32_t pkts = rx_.c.pkts;
        portEXIT_CRITICAL(&mux_);
        if (pkts != last_pkts)
        {
            last_pkts = pkts;
            quiet_ms = 0;
            continue;
        }
        quiet_ms += k_slice_ms;
        if (quiet_ms >= k_progress_timeout_ms)
        {
            // Stop the P4's sender too, or it runs on to `count` and the next
            // requests queue behind it.
            if (!stop_sent)
            {
                send_bw_tx_stop();
            }
            if (wait_reply(key, k_reply_timeout_ms))
            {
                memcpy(done, reply_, reply_len_ < sizeof(*done) ? reply_len_ : sizeof(*done));
            }
            *error = "p4_tx_timeout";
            return false;
        }
    }
}

void sdio_bwtest::finish(sdio_test_state state, const char* error)
{
    xSemaphoreTake(result_mu_, portMAX_DELAY);
    result_.state = state;
    result_.error = error;
    xSemaphoreGive(result_mu_);
}

void sdio_bwtest::run()
{
    const sdio_test_request req = req_;
    const uint8_t p4_probe = wire_probe(req.probe);
    const uint8_t c6_probe =
        req.probe == sdio_test_probe::internal ? SDIO_TEST_PROBE_INTERNAL : SDIO_TEST_PROBE_OFF;
    sdio_test_result r = result();
    const char* error = nullptr;
    sdio_test_rx_stats_t st{};
    sdio_test_meter_mark_t mark;
    sdio_test_window_t w;

    if (sdio_test_probe_set(c6_probe) != ESP_OK)
    {
        ESP_LOGW(TAG, "C6 probe not running");
    }
    // Open the P4's window (and start its probe).
    if (!get_p4_window(SDIO_TEST_FLAG_CLEAR, p4_probe, req.axi_agent, &st))
    {
        error = "p4_no_reply";
    }
    sdio_test_meter_mark(&mark);

    if (error == nullptr && req.base_ms > 0)
    {
        // In slices, so a stop does not wait out a long baseline.
        for (uint32_t ms = 0; ms < req.base_ms && !stop_.load(); ms += 50)
        {
            vTaskDelay(pdMS_TO_TICKS(req.base_ms - ms < 50 ? req.base_ms - ms : 50));
        }
        sdio_test_meter_read(&mark, &w);
        r.c6_base = to_stats(w);
        if (get_p4_window(SDIO_TEST_FLAG_CLEAR, p4_probe, req.axi_agent, &st))
        {
            r.p4_base = to_stats(st.window);
        }
        else
        {
            error = "p4_no_reply";
        }
        sdio_test_meter_mark(&mark);
    }

    if (error == nullptr && !stop_.load())
    {
        portENTER_CRITICAL(&mux_);
        sdio_test_rx_reset(&rx_, run_);
        portEXIT_CRITICAL(&mux_);

        sdio_test_bw_tx_done_t done{};
        bool p4_sending = false;
        uint32_t done_key = 0;
        if (sends_p4(req.dir))
        {
            sdio_test_bw_tx_t m{};
            m.type = SDIO_TEST_BW_TX;
            m.run = run_;
            m.mtu = req.mtu;
            m.count = req.count;
            m.pacing_us = req.pacing_us;
            // Expect the done before sending: a short run may finish at once.
            done_key = arm(SDIO_TEST_BW_TX_DONE);
            p4_sending = send(&m, sizeof(m));
            if (!p4_sending)
            {
                expect_.store(0);
                error = "p4_no_bw_tx";
            }
        }
        if (sends_c6(req.dir))
        {
            r.c6p4.ran = true;
            generate(&r.c6p4);
        }
        if (p4_sending && !wait_bw_tx_done(done_key, &done, &error))
        {
            p4_sending = false;
        }

        sdio_test_meter_read(&mark, &w);
        r.c6_load = to_stats(w);
        if (get_p4_window(SDIO_TEST_FLAG_CLEAR, SDIO_TEST_PROBE_OFF, nullptr, &st))
        {
            r.p4_load = to_stats(st.window);
            r.p4_credit_waits = st.link_credit_waits;
            r.p4_rx_reads = st.link_rx_reads;
            r.p4_tx_writes = st.link_tx_writes;
            r.p4_link_freq_khz = st.link_freq_khz;
            r.p4_link_downs = st.link_downs;
            r.p4_tx_slots = st.link_tx_slots;
            r.p4_tx_msgs = st.link_tx_msgs;
            if (r.c6p4.ran)
            {
                r.c6p4.rx = st.rx.pkts;
                r.c6p4.lost = st.rx.lost;
                r.c6p4.reorder = st.rx.reorder;
                r.c6p4.bad = st.rx.bad;
                r.c6p4.rx_us = st.rx.span_us;
                r.c6p4.rx_bytes = st.rx.bytes;
            }
        }
        else if (error == nullptr)
        {
            error = "p4_no_reply";
        }
        if (sends_p4(req.dir))
        {
            r.p4c6.ran = true;
            r.p4c6.sent = done.sent;
            r.p4c6.send_fail = done.send_fail;
            r.p4c6.sender_us = done.elapsed_us;
            portENTER_CRITICAL(&mux_);
            const sdio_test_rx_counts_t c = rx_.c;
            portEXIT_CRITICAL(&mux_);
            r.p4c6.rx = c.pkts;
            r.p4c6.lost = c.lost;
            r.p4c6.reorder = c.reorder;
            r.p4c6.bad = c.bad;
            r.p4c6.rx_us = c.span_us;
            r.p4c6.rx_bytes = c.bytes;
        }
    }
    else
    {
        // Failed or stopped before the traffic. Best effort: the P4 probe may
        // be running.
        get_p4_window(SDIO_TEST_FLAG_CLEAR, SDIO_TEST_PROBE_OFF, nullptr, &st);
    }
    sdio_test_probe_set(SDIO_TEST_PROBE_OFF);

    const sdio_test_state state = error != nullptr ? sdio_test_state::failed
                                  : stop_.load()   ? sdio_test_state::stopped
                                                   : sdio_test_state::done;
    xSemaphoreTake(result_mu_, portMAX_DELAY);
    r.state = state;
    r.error = error;
    result_ = r;
    xSemaphoreGive(result_mu_);
    ESP_LOGI(TAG, "run %u %s%s%s", static_cast<unsigned>(run_),
             state == sdio_test_state::done ? "done" : state == sdio_test_state::stopped ? "stopped"
                                                                                       : "failed",
             error ? " " : "", error ? error : "");
}
