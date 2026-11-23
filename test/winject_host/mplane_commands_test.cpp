#include "mplane_commands.h"

#include "mplane_reply.h"

#include <gtest/gtest.h>

#include <string>
#include <string.h>

namespace
{
uint32_t ip(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    uint32_t out = 0;
    auto* p = reinterpret_cast<uint8_t*>(&out);
    p[0] = a;
    p[1] = b;
    p[2] = c;
    p[3] = d;
    return out;
}

class string_reply : public mplane_reply
{
public:
    using mplane_reply::write;

    void write(const char* data, size_t n) override
    {
        text.append(data, n);
    }

    std::string text;
};

class fake_device : public mplane_device_backend
{
public:
    fake_device()
    {
        network_.ip = ip(192, 168, 32, 1);
        tune_.eth_rx_ring_sz = 28;
        tune_.eth_tx_ring_sz = 16;
        tune_.wifi_tx_ring_sz = 16;
        tune_.wifi_rx_ring_sz = 32;
    }

    mplane_status restart(std::optional<WinjectMode> mode) override
    {
        ++restarts;
        restart_mode = mode;
        return mplane_status::ok;
    }

    network_config network() const override
    {
        return network_;
    }

    mplane_status set_network(const network_config& cfg) override
    {
        network_ = cfg;
        return mplane_status::ok;
    }

    tune_config tune() const override
    {
        return tune_;
    }

    mplane_status set_tune(const tune_config& cfg) override
    {
        tune_limits l;
        l.eth_rx_ring_sz = 28;
        l.eth_tx_ring_sz = 16;
        l.wifi_rx_ring_min = 8;
        if (!tune_config_valid(cfg, l))
        {
            return mplane_status::invalid;
        }
        tune_ = cfg;
        return mplane_status::ok;
    }

    mplane_status save(uint8_t slot) override
    {
        saved_slot = slot;
        return mplane_status::ok;
    }

    mplane_status load(uint8_t slot) override
    {
        return slot == 3 ? mplane_status::ok : mplane_status::not_found;
    }

    int64_t uptime_us() const override
    {
        return uptime_us_;
    }

    const char* version() const override
    {
        return WINJECT_VERSION_STRING;
    }

    int restarts = 0;
    int64_t uptime_us_ = 123456789;
    std::optional<WinjectMode> restart_mode;
    int saved_slot = -1;

private:
    network_config network_;
    tune_config tune_;
};

class fake_radio : public mplane_radio_backend
{
public:
    uint8_t tx_queue_size() const override
    {
        return 3;
    }
    uint8_t tx_in_flight() const override
    {
        return 2;
    }
    uint8_t rx_queue_size() const override
    {
        return 1;
    }

    uint32_t tx_dropped_invalid_frame() const override
    {
        return tx_dropped_invalid_frame_;
    }
    uint32_t tx_dropped_tx_queue() const override
    {
        return tx_dropped_tx_queue_;
    }
    uint32_t tx_dropped_wifi() const override
    {
        return tx_dropped_wifi_;
    }
    uint32_t rx_dropped_filter_mismatched() const override
    {
        return rx_dropped_filter_mismatched_;
    }
    uint32_t rx_dropped_rx_queue() const override
    {
        return rx_dropped_rx_queue_;
    }
    uint32_t rx_dropped_no_peer() const override
    {
        return rx_dropped_no_peer_;
    }
    uint32_t rx_dropped_send_failed() const override
    {
        return rx_dropped_send_failed_;
    }
    uint32_t tx_ether_pkt() const override
    {
        return tx_ether_pkt_;
    }
    uint32_t rx_ether_pkt() const override
    {
        return rx_ether_pkt_;
    }
    uint32_t tx_air_pkt() const override
    {
        return tx_air_pkt_;
    }
    uint32_t rx_air_pkt() const override
    {
        return rx_air_pkt_;
    }

    radio_config radio() const override
    {
        return radio_;
    }

    bool rx_rssi(int8_t* dbm) const override
    {
        if (!have_rssi)
        {
            return false;
        }
        *dbm = -42;
        return true;
    }

    bool temperature_c_x10(int16_t* out) const override
    {
        if (!have_temp)
        {
            return false;
        }
        *out = temp_c_x10;
        return true;
    }

    bool tx_power_applied_dbm(int8_t* dbm) const override
    {
        if (!have_tx_power_readback)
        {
            return false;
        }
        *dbm = 15;
        return true;
    }

    radio_caps caps() const override
    {
        return radio_caps{fcs_mode_};
    }

    mplane_status set_radio(const radio_patch& patch) override
    {
        ++set_radio_calls;
        if (patch.modulation != nullptr)
        {
            if (strcmp(patch.modulation, "BOGUS") == 0)
            {
                return mplane_status::invalid;
            }
            strncpy(radio_.modulation, patch.modulation,
                    sizeof(radio_.modulation) - 1);
        }
        if (patch.channel)
        {
            radio_.channel = *patch.channel;
        }
        if (patch.tx_power_dbm)
        {
            radio_.tx_power_dbm = *patch.tx_power_dbm;
        }
        if (patch.cca_enabled)
        {
            radio_.cca_enabled = *patch.cca_enabled;
        }
        return mplane_status::ok;
    }

    size_t tx_cal_groups() const override
    {
        return 2;
    }

    mplane_status tx_cal(size_t index, radio_tx_cal_curve* out) const override
    {
        if (index >= 2)
        {
            return mplane_status::invalid;
        }
        *out = cal_[index];
        return mplane_status::ok;
    }

    mplane_status set_tx_cal(const radio_tx_cal_patch& patch) override
    {
        last_tx_cal = patch;
        last_tx_cal_group = patch.group != nullptr ? patch.group : "";
        ++set_tx_cal_calls;
        if (patch.group != nullptr && strcmp(patch.group, "dsss") != 0 &&
            strcmp(patch.group, "ofdm") != 0)
        {
            return mplane_status::invalid;
        }
        if (!patch.reset)
        {
            const size_t i = strcmp(patch.group, "dsss") == 0 ? 0 : 1;
            const char* g = cal_[i].group;
            cal_[i] = patch.curve;
            cal_[i].group = g;
            cal_[i].is_default = false;
        }
        return mplane_status::ok;
    }

    mac_filter rx_filter_addr3() const override
    {
        return filter_;
    }

    mplane_status set_rx_filter_addr3(const mac_filter& filter) override
    {
        filter_ = filter;
        return mplane_status::ok;
    }

    const char* modulation_list() const override
    {
        return "DSS_1M_L OFDM_6M";
    }

    bool have_rssi = false;
    bool have_tx_power_readback = false;
    bool have_temp = false;
    int16_t temp_c_x10 = 0;
    int set_radio_calls = 0;
    int set_tx_cal_calls = 0;
    radio_tx_cal_patch last_tx_cal;
    std::string last_tx_cal_group;  // patch.group points into the request
    radio_tx_cal_curve cal_[2] = {{"dsss", 2, {{8, -2930}, {88, 1310}}, true},
                                  {"ofdm", 2, {{8, -2930}, {88, 1310}}, true}};
    fcs_mode fcs_mode_ = fcs_mode::signal;
    uint32_t tx_dropped_invalid_frame_ = 3;
    uint32_t tx_dropped_tx_queue_ = 4;
    uint32_t tx_dropped_wifi_ = 5;
    uint32_t rx_dropped_filter_mismatched_ = 14;
    uint32_t rx_dropped_rx_queue_ = 7;
    uint32_t rx_dropped_no_peer_ = 8;
    uint32_t rx_dropped_send_failed_ = 9;
    uint32_t tx_ether_pkt_ = 12;
    uint32_t rx_ether_pkt_ = 13;
    uint32_t tx_air_pkt_ = 10;
    uint32_t rx_air_pkt_ = 11;

private:
    radio_config radio_;
    mac_filter filter_;
};

class fake_test : public mplane_test_backend
{
public:
    mplane_status set_ether_rx_port(uint16_t port) override
    {
        ether_rx_port = port;
        return mplane_status::ok;
    }

    rx_test_stats ether_rx_stats(bool clear) override
    {
        last_clear = clear;
        rx_test_stats s;
        s.pkt = 10;
        s.byt = 14000;
        return s;
    }

    mplane_status ether_tx(const ether_tx_request& req) override
    {
        ether_req = req;
        return next_status;
    }

    mplane_status set_wifi_rx_match(const wifi_rx_match& match) override
    {
        wifi_match = match;
        return mplane_status::ok;
    }

    rx_test_stats wifi_rx_stats(bool clear) override
    {
        last_clear = clear;
        rx_test_stats s;
        s.pkt = 5;
        s.byt = 500;
        s.fec_error_pkt = 1;
        return s;
    }

    mplane_status wifi_tx(const wifi_tx_request& req) override
    {
        wifi_req = req;
        return next_status;
    }

    mplane_status sdio_test(const sdio_test_request& req) override
    {
        sdio_req = req;
        ++sdio_calls;
        return next_status;
    }

    mplane_status sdio_test_stat(sdio_test_result* out) const override
    {
        *out = sdio_result;
        return mplane_status::ok;
    }

    sdio_test_request sdio_req;
    int sdio_calls = 0;
    sdio_test_result sdio_result;
    uint16_t ether_rx_port = 1;
    bool last_clear = false;
    ether_tx_request ether_req;
    wifi_rx_match wifi_match;
    wifi_tx_request wifi_req;
    mplane_status next_status = mplane_status::ok;
};

class MplaneCommandsTest : public ::testing::Test
{
protected:
    std::string run(const char* line)
    {
        std::string buf(line);
        string_reply reply;
        commands.handle_text(buf.data(), reply);
        return reply.text;
    }

    fake_device device;
    fake_radio radio;
    fake_test test;
    mplane_commands commands{device, &radio, &test};
};
}  // namespace

TEST_F(MplaneCommandsTest, PingAndAlias)
{
    EXPECT_EQ(run("ping"), "pong\n");
    EXPECT_EQ(run("P\r\n"), "pong\n");
    EXPECT_EQ(run("ping extra"), "NOK EINVAL\n");
}

// frozen: changing this breaks version discovery for every older and newer peer.
TEST_F(MplaneCommandsTest, VersionCommand)
{
    EXPECT_EQ(run("version"), "OK version ver=v1.0.0 proto=1.0\n");
    EXPECT_EQ(run("ver"), "OK version ver=v1.0.0 proto=1.0\n");
    EXPECT_EQ(run("version x=1"), "NOK EINVAL\n");
    EXPECT_EQ(run("cmd:7 version"), "OK:7 version ver=v1.0.0 proto=1.0\n");
}

TEST_F(MplaneCommandsTest, UnknownCommandAndComments)
{
    EXPECT_EQ(run("status"), "NOK ENOSYS\n");
    EXPECT_EQ(run("# comment"), "");
    EXPECT_EQ(run(""), "");
}

TEST_F(MplaneCommandsTest, MultipleLinesInOneDatagram)
{
    EXPECT_EQ(
        run("ping\nti\n"),
        "pong\ntx_info tx_queue_sz=3 in_flight=2 dropped_invalid_frame=3 "
        "dropped_tx_queue=4 dropped_wifi=5 ether_pkt=12 air_pkt=10 "
        "ts=123456789\n");
}

TEST_F(MplaneCommandsTest, HelpListsCommandsAndModulations)
{
    const std::string out = run("?");
    EXPECT_NE(out.find("tune_rx_param|trp "), std::string::npos);
    EXPECT_NE(out.find("save <slot"), std::string::npos);
    EXPECT_NE(out.find("modulations: DSS_1M_L OFDM_6M\n"), std::string::npos);
}

TEST_F(MplaneCommandsTest, ResetWithAndWithoutMode)
{
    EXPECT_EQ(run("reset"), "OK\n");
    EXPECT_FALSE(device.restart_mode.has_value());
    EXPECT_EQ(run("r mode=ota"), "OK\n");
    ASSERT_TRUE(device.restart_mode.has_value());
    EXPECT_EQ(*device.restart_mode, WINJECT_MODE_OTA);
    EXPECT_EQ(run("r mode=WINJECT"), "OK\n");
    EXPECT_EQ(*device.restart_mode, WINJECT_MODE_STANDALONE);
    EXPECT_EQ(run("reset mode=STANDALONE"), "NOK EINVAL\n");
    EXPECT_EQ(run("reset id=3"), "NOK EINVAL\n");
    EXPECT_EQ(device.restarts, 3);
}

TEST_F(MplaneCommandsTest, SaveAndLoadSlots)
{
    EXPECT_EQ(run("save 4"), "OK\n");
    EXPECT_EQ(device.saved_slot, 4);
    EXPECT_EQ(run("save"), "NOK EINVAL\n");
    EXPECT_EQ(run("save 10"), "NOK EINVAL\n");
    EXPECT_EQ(run("save 1 2"), "NOK EINVAL\n");
    EXPECT_EQ(run("load 3"), "OK\n");
    EXPECT_EQ(run("load 2"), "NOK ENOENT\n");
}

TEST_F(MplaneCommandsTest, SetNetworkQueryAndUpdate)
{
    EXPECT_EQ(run("sn"),
              "OK network ip=192.168.32.1/24 type=dhcp timeout=5\n");
    EXPECT_EQ(run("network ip=10.1.0.9/16 type=static timeout=0"),
              "OK network ip=10.1.0.9/16 type=static timeout=0\n");
    EXPECT_EQ(device.network().ip, ip(10, 1, 0, 9));
    EXPECT_EQ(device.network().type, network_type::static_ip);
    // Prefix kept when omitted.
    EXPECT_EQ(run("sn ip=10.1.0.10"),
              "OK network ip=10.1.0.10/16 type=static timeout=0\n");
}

TEST_F(MplaneCommandsTest, SetNetworkRejectsBadValues)
{
    EXPECT_EQ(run("sn type=auto"), "NOK EINVAL\n");
    EXPECT_EQ(run("sn ip=10.1.0.0/16"), "NOK EINVAL\n");
    EXPECT_EQ(run("sn ip=10.1.0.300/16"), "NOK EINVAL\n");
    EXPECT_EQ(run("sn timeout=70000"), "NOK EINVAL\n");
    EXPECT_EQ(run("sn mode=static"), "NOK EINVAL\n");
    EXPECT_EQ(device.network().ip, ip(192, 168, 32, 1));
}

TEST_F(MplaneCommandsTest, TuneCommandsUpdateAndEcho)
{
    EXPECT_EQ(run("tp set_eth_dma_burst_len=8"),
              "OK tune_param set_eth_dma_burst_len=8\n");
    EXPECT_EQ(run("ttp tx_queue_sz=40 wifi_tx_ring_sz=24"),
              "OK tune_tx_param eth_rx_ring_sz=28 tx_queue_sz=40 "
              "wifi_tx_ring_sz=24\n");
    EXPECT_EQ(run("tune_rx_param rx_queue_sz=12"),
              "OK tune_rx_param eth_tx_ring_sz=16 rx_queue_sz=12 "
              "wifi_rx_ring_sz=32\n");
    EXPECT_EQ(run("trp"),
              "OK tune_rx_param eth_tx_ring_sz=16 rx_queue_sz=12 "
              "wifi_rx_ring_sz=32\n");
}

TEST_F(MplaneCommandsTest, TuneCommandsRejectInvalid)
{
    EXPECT_EQ(run("tp set_eth_dma_burst_len=3"), "NOK EINVAL\n");
    EXPECT_EQ(run("ttp eth_rx_ring_sz=10"), "NOK EINVAL\n");
    EXPECT_EQ(run("ttp tx_queue_sz=256"), "NOK EINVAL\n");
    EXPECT_EQ(run("ttp rx_queue_sz=4"), "NOK EINVAL\n");
    EXPECT_EQ(run("trp wifi_rx_ring_sz=2"), "NOK EINVAL\n");
    EXPECT_EQ(device.tune().eth_dma_burst_len, 32);
}

TEST_F(MplaneCommandsTest, QueueInfo)
{
    EXPECT_EQ(
        run("tx_info"),
        "tx_info tx_queue_sz=3 in_flight=2 dropped_invalid_frame=3 "
        "dropped_tx_queue=4 dropped_wifi=5 ether_pkt=12 air_pkt=10 "
        "ts=123456789\n");
    EXPECT_EQ(
        run("ri"),
        "rx_info rx_queue_sz=1 dropped_filter_mismatched=14 "
        "dropped_rx_queue=7 dropped_no_peer=8 dropped_send_failed=9 "
        "ether_pkt=13 air_pkt=11 ts=123456789\n");
    EXPECT_EQ(run("ri now=1"), "NOK EINVAL\n");
}

TEST_F(MplaneCommandsTest, QueueInfoLargeCountersAndUptime)
{
    radio.tx_dropped_invalid_frame_ = 4294967295u;
    radio.tx_dropped_tx_queue_ = 4294967295u;
    radio.tx_dropped_wifi_ = 4294967295u;
    radio.tx_ether_pkt_ = 4294967295u;
    radio.tx_air_pkt_ = 4294967295u;
    device.uptime_us_ = 5000000000LL;
    EXPECT_EQ(
        run("tx_info"),
        "tx_info tx_queue_sz=3 in_flight=2 dropped_invalid_frame=4294967295 "
        "dropped_tx_queue=4294967295 dropped_wifi=4294967295 "
        "ether_pkt=4294967295 air_pkt=4294967295 ts=5000000000\n");
}

TEST_F(MplaneCommandsTest, RadioTxPartialUpdate)
{
    EXPECT_EQ(run("rt channel=6 cca=false"),
              "OK radio_tx channel=6 tx_power=20 modulation=DSS_1M_L "
              "cca=false\n");
    EXPECT_EQ(run("radio_tx modulation=OFDM_6M tx_power=13"),
              "OK radio_tx channel=6 tx_power=13 modulation=OFDM_6M "
              "cca=false\n");
}

TEST_F(MplaneCommandsTest, RadioTxInvalidArgument)
{
    EXPECT_EQ(run("rt channel=15"), "NOK EINVAL\n");
    EXPECT_EQ(run("rt tx_power=31"), "NOK EINVAL\n");
    EXPECT_EQ(run("rt tx_power=-31"), "NOK EINVAL\n");
    EXPECT_EQ(run("rt cca=maybe"), "NOK EINVAL\n");
    EXPECT_EQ(run("rt modulation="), "NOK EINVAL\n");
    EXPECT_EQ(run("rt modulation=BOGUS"), "NOK EINVAL\n");
    EXPECT_EQ(run("rt power=3"), "NOK EINVAL\n");
    EXPECT_EQ(radio.set_radio_calls, 1);
}

TEST_F(MplaneCommandsTest, RadioTxInfoWithOptionalRssi)
{
    EXPECT_EQ(run("rti"),
              "radio_tx channel=1 tx_power=20 modulation=DSS_1M_L cca=true\n");
    radio.have_rssi = true;
    EXPECT_EQ(run("radio_tx_info"),
              "radio_tx channel=1 tx_power=20 modulation=DSS_1M_L cca=true\n"
              "radio_rx rssi=-42\n");
    // The read-back is an extra key on the radio_tx line (managers ignore it):
    // normalized dBm in force, here below the request (radio maximum).
    radio.have_tx_power_readback = true;
    EXPECT_EQ(run("radio_tx_info"),
              "radio_tx channel=1 tx_power=20 modulation=DSS_1M_L cca=true tx_power_applied=15\n"
              "radio_rx rssi=-42\n");
    EXPECT_EQ(run("radio_tx tx_power=20"),
              "OK radio_tx channel=1 tx_power=20 modulation=DSS_1M_L cca=true tx_power_applied=15\n");
}

TEST_F(MplaneCommandsTest, RadioTxInfoWithTemperature)
{
    EXPECT_EQ(run("rti"),
              "radio_tx channel=1 tx_power=20 modulation=DSS_1M_L cca=true\n");
    // Chip temperature in 0.1 degC, printed only when the radio has a sensor.
    radio.have_temp = true;
    radio.temp_c_x10 = 423;
    EXPECT_EQ(run("rti"),
              "radio_tx channel=1 tx_power=20 modulation=DSS_1M_L cca=true\n"
              "radio_temp c=42.3\n");
    radio.temp_c_x10 = -35;
    EXPECT_EQ(run("rti"),
              "radio_tx channel=1 tx_power=20 modulation=DSS_1M_L cca=true\n"
              "radio_temp c=-3.5\n");
}

TEST_F(MplaneCommandsTest, RadioCapsInfo)
{
    EXPECT_EQ(run("radio_caps_info"), "OK radio_caps_info fcs=SIGNAL\n");
    EXPECT_EQ(run("rci"), "OK radio_caps_info fcs=SIGNAL\n");
    EXPECT_EQ(run("radio_caps_info x=1"), "NOK EINVAL\n");
    radio.fcs_mode_ = fcs_mode::actual;
    EXPECT_EQ(run("radio_caps_info"), "OK radio_caps_info fcs=ACTUAL\n");
    EXPECT_EQ(run("cmd:9 radio_caps_info"), "OK:9 radio_caps_info fcs=ACTUAL\n");
    mplane_commands ota(device, nullptr, nullptr);
    string_reply reply;
    std::string line = "radio_caps_info";
    ota.handle_line(line.data(), reply);
    EXPECT_EQ(reply.text, "NOK ENODEV\n");
}

TEST_F(MplaneCommandsTest, RxFilterAddr3SetQueryClear)
{
    EXPECT_EQ(run("rf3 addr=CA:FE:BA:BE:00:01"),
              "OK rx_filter_addr3 addr=ca:fe:ba:be:00:01\n");
    EXPECT_TRUE(radio.rx_filter_addr3().enabled);
    EXPECT_EQ(run("rf3"), "OK rx_filter_addr3 addr=ca:fe:ba:be:00:01\n");
    EXPECT_EQ(run("rx_filter_addr3 addr="), "OK rx_filter_addr3 addr=\n");
    EXPECT_FALSE(radio.rx_filter_addr3().enabled);
    EXPECT_EQ(run("rf3 addr=nope"), "NOK EINVAL\n");
}

TEST_F(MplaneCommandsTest, OtaModeRejectsRadioAndTestCommands)
{
    mplane_commands ota(device, nullptr, nullptr);
    string_reply reply;
    std::string a = "ti";
    ota.handle_line(a.data(), reply);
    std::string b = "test_wifi_rx_stat";
    ota.handle_line(b.data(), reply);
    std::string c = "ping";
    ota.handle_line(c.data(), reply);
    EXPECT_EQ(reply.text, "NOK ENODEV\nNOK ENODEV\npong\n");
}

TEST_F(MplaneCommandsTest, EtherRxPort)
{
    EXPECT_EQ(run("ter port=5001"), "OK ether_rx port=5001\n");
    EXPECT_EQ(test.ether_rx_port, 5001);
    EXPECT_EQ(run("test_ether_rx port=0"), "OK ether_rx port=0\n");
    EXPECT_EQ(test.ether_rx_port, 0);
    EXPECT_EQ(run("ter"), "NOK EINVAL\n");
}

TEST_F(MplaneCommandsTest, EtherTxStartAndStop)
{
    EXPECT_EQ(
        run("tet id=7 host=192.168.1.2 port=5001 mtu=1400 count=100 rate=8000"),
        "OK\n");
    EXPECT_EQ(test.ether_req.id, 7);
    EXPECT_EQ(test.ether_req.host, ip(192, 168, 1, 2));
    EXPECT_EQ(test.ether_req.port, 5001);
    EXPECT_EQ(test.ether_req.mtu, 1400);
    EXPECT_EQ(test.ether_req.count, 100);
    EXPECT_EQ(test.ether_req.rate_kbps, 8000u);

    EXPECT_EQ(run("tet id=7 count=0"), "OK\n");
    EXPECT_EQ(test.ether_req.count, 0);

    test.next_status = mplane_status::already;
    EXPECT_EQ(run("tet id=8 host=1.2.3.4 port=1 mtu=10 count=1"),
              "NOK EALREADY\n");
    test.next_status = mplane_status::stale;
    EXPECT_EQ(run("tet id=9 count=0"), "NOK ESTALE\n");
}

TEST_F(MplaneCommandsTest, EtherTxRejectsMissingOrBadFields)
{
    EXPECT_EQ(run("tet host=1.2.3.4 port=1 mtu=10 count=1"), "NOK EINVAL\n");
    EXPECT_EQ(run("tet id=1 port=1 mtu=10 count=1"), "NOK EINVAL\n");
    EXPECT_EQ(run("tet id=1 host=1.2.3.4 port=1 mtu=1473 count=1"),
              "NOK EINVAL\n");
    EXPECT_EQ(run("tet id=1 host=1.2.3.4 port=0 mtu=10 count=1"),
              "NOK EINVAL\n");
}

TEST_F(MplaneCommandsTest, RxStatsWithClear)
{
    EXPECT_EQ(run("ters"), "test_ether_rx_stat pkt=10 byt=14000\n");
    EXPECT_FALSE(test.last_clear);
    EXPECT_EQ(run("ters clear=true"), "test_ether_rx_stat pkt=10 byt=14000\n");
    EXPECT_TRUE(test.last_clear);
    EXPECT_EQ(run("twrs clear=1"),
              "test_wifi_rx_stat pkt=5 byt=500 fec_error_pkt=1\n");
    EXPECT_EQ(run("twrs clear=x"), "NOK EINVAL\n");
}

TEST_F(MplaneCommandsTest, WifiRxMatchAndStop)
{
    EXPECT_EQ(run("twr addr3=ca:fe:ba:be:00:01 addr1="), "OK\n");
    EXPECT_FALSE(test.wifi_match.addr1.enabled);
    EXPECT_FALSE(test.wifi_match.addr2.enabled);
    EXPECT_TRUE(test.wifi_match.addr3.enabled);
    EXPECT_TRUE(test.wifi_match.active());
    EXPECT_EQ(run("test_wifi_rx"), "OK\n");
    EXPECT_FALSE(test.wifi_match.active());
    EXPECT_EQ(run("twr addr2=xyz"), "NOK EINVAL\n");
}

TEST_F(MplaneCommandsTest, CmdPrefixCorrelation)
{
    EXPECT_EQ(run("cmd:3 ping"), "OK:3 pong\n");
    EXPECT_EQ(run("cmd:4 save 1"), "OK:4\n");
    EXPECT_EQ(run("cmd:5 radio_tx channel=6"),
              "OK:5 radio_tx channel=6 tx_power=20 modulation=DSS_1M_L "
              "cca=true\n");
    EXPECT_EQ(run("cmd:6 bogus"), "NOK:6 ENOSYS\n");
    EXPECT_EQ(run("cmd:x ping"), "NOK EINVAL\n");
    EXPECT_EQ(run("cmd:7"), "NOK EINVAL\n");
    EXPECT_EQ(run("ping"), "pong\n");
    EXPECT_EQ(run("cmd:8 reset"), "OK:8\n");
}

TEST_F(MplaneCommandsTest, WifiTxStartAndStop)
{
    EXPECT_EQ(run("twt addr3=ca:fe:ba:be:00:01 mtu=200 count=50 rate=1000"),
              "OK\n");
    EXPECT_FALSE(test.wifi_req.id.has_value());
    EXPECT_FALSE(test.wifi_req.addr1.has_value());
    ASSERT_TRUE(test.wifi_req.addr3.has_value());
    EXPECT_EQ((*test.wifi_req.addr3)[5], 0x01);
    EXPECT_EQ(test.wifi_req.mtu, 200);
    EXPECT_EQ(test.wifi_req.count, 50);
    EXPECT_EQ(test.wifi_req.rate_kbps, 1000u);

    EXPECT_EQ(run("twt id=2 count=0"), "OK\n");
    ASSERT_TRUE(test.wifi_req.id.has_value());
    EXPECT_EQ(*test.wifi_req.id, 2);

    EXPECT_EQ(run("twt mtu=23 count=1"), "NOK EINVAL\n");
    EXPECT_EQ(run("twt mtu=100"), "NOK EINVAL\n");
}

TEST_F(MplaneCommandsTest, RadioTxCalListsGroups)
{
    EXPECT_EQ(run("radio_tx_cal"),
              "OK radio_tx_cal group=dsss default=true points=8:-29.30,88:13.10\n"
              "radio_tx_cal group=ofdm default=true points=8:-29.30,88:13.10\n"
              "radio_tx channel=1 tx_power=20 modulation=DSS_1M_L cca=true\n");
    EXPECT_EQ(radio.set_tx_cal_calls, 0);
}

TEST_F(MplaneCommandsTest, RadioTxCalSetsPoints)
{
    EXPECT_EQ(run("rtc group=dsss points=8:-28,66:2.6,72:9.5,84:15.62"),
              "OK radio_tx_cal group=dsss default=false points=8:-28.00,66:2.60,72:9.50,84:15.62\n"
              "radio_tx_cal group=ofdm default=true points=8:-29.30,88:13.10\n"
              "radio_tx channel=1 tx_power=20 modulation=DSS_1M_L cca=true\n");
    EXPECT_EQ(radio.last_tx_cal_group, "dsss");
    EXPECT_FALSE(radio.last_tx_cal.reset);
    EXPECT_EQ(radio.last_tx_cal.curve.n, 4);
    EXPECT_EQ(radio.last_tx_cal.curve.p[1].drv, 66);
    EXPECT_EQ(radio.last_tx_cal.curve.p[1].dbm_x100, 260);
    EXPECT_EQ(radio.last_tx_cal.curve.p[3].dbm_x100, 1562);
}

TEST_F(MplaneCommandsTest, RadioTxCalScaleOffsetIsALine)
{
    EXPECT_EQ(run("rtc group=ofdm scale=2.02 offset=-26.6").substr(0, 3), "OK ");
    EXPECT_EQ(radio.last_tx_cal.curve.n, 2);
    EXPECT_EQ(radio.last_tx_cal.curve.p[0].drv, 8);
    EXPECT_EQ(radio.last_tx_cal.curve.p[0].dbm_x100, -2256);  // -26.6 + 8 * 2.02 / 4
    EXPECT_EQ(radio.last_tx_cal.curve.p[1].drv, 88);
    EXPECT_EQ(radio.last_tx_cal.curve.p[1].dbm_x100, 1784);
}

TEST_F(MplaneCommandsTest, RadioTxCalReset)
{
    EXPECT_EQ(run("rtc reset=true").substr(0, 3), "OK ");
    EXPECT_EQ(radio.last_tx_cal_group, "");
    EXPECT_TRUE(radio.last_tx_cal.reset);
    EXPECT_EQ(run("rtc group=ofdm reset=true").substr(0, 3), "OK ");
    EXPECT_EQ(radio.last_tx_cal_group, "ofdm");
    EXPECT_TRUE(radio.last_tx_cal.reset);
}

TEST_F(MplaneCommandsTest, RadioTxCalRejectsBadRequests)
{
    EXPECT_EQ(run("rtc points=8:-28,84:15"), "NOK EINVAL\n");                // no group
    EXPECT_EQ(run("rtc group=dsss"), "NOK EINVAL\n");                        // nothing to set
    EXPECT_EQ(run("rtc group=dsss scale=2"), "NOK EINVAL\n");                // half a line
    EXPECT_EQ(run("rtc group=dsss points=8:-28 scale=2 offset=-26"), "NOK EINVAL\n");
    EXPECT_EQ(run("rtc group=dsss points=8-28,84:15"), "NOK EINVAL\n");      // no colon
    EXPECT_EQ(run("rtc group=dsss points=8:-28.123,84:15"), "NOK EINVAL\n"); // 3 decimals
    EXPECT_EQ(run("rtc group=dsss points=300:1,84:15"), "NOK EINVAL\n");     // drv > 255
    EXPECT_EQ(run("rtc group=dsss points=1:1,2:2,3:3,4:4,5:5,6:6,7:7,8:8,9:9,10:10,11:11,12:12,13:13"),
              "NOK EINVAL\n");                                               // 13 points
    EXPECT_EQ(run("rtc group= reset=true"), "NOK EINVAL\n");
    EXPECT_EQ(run("rtc gain=1"), "NOK EINVAL\n");
    EXPECT_EQ(radio.set_tx_cal_calls, 0);
    // The backend owns group names and curve rules.
    EXPECT_EQ(run("rtc group=bogus points=8:-28,84:15"), "NOK EINVAL\n");
    EXPECT_EQ(radio.set_tx_cal_calls, 1);
}

TEST_F(MplaneCommandsTest, TestSdioParses)
{
    EXPECT_EQ(run("test_sdio dir=both mtu=1400 count=1000 pacing_us=250 probe=int base_ms=0"),
              "OK\n");
    EXPECT_EQ(test.sdio_req.dir, sdio_test_dir::both);
    EXPECT_EQ(test.sdio_req.mtu, 1400u);
    EXPECT_EQ(test.sdio_req.count, 1000u);
    EXPECT_EQ(test.sdio_req.pacing_us, 250u);
    EXPECT_EQ(test.sdio_req.probe, sdio_test_probe::internal);
    EXPECT_EQ(test.sdio_req.base_ms, 0u);
    EXPECT_EQ(run("tsd dir=c6p4 mtu=8 count=1"), "OK\n");
    EXPECT_EQ(test.sdio_req.dir, sdio_test_dir::c6p4);
    EXPECT_EQ(test.sdio_req.probe, sdio_test_probe::off);
    EXPECT_EQ(test.sdio_req.base_ms, 500u);
    EXPECT_EQ(run("test_sdio count=0"), "OK\n");
    EXPECT_EQ(test.sdio_req.count, 0u);
}

TEST_F(MplaneCommandsTest, TestSdioAxiPorts)
{
    EXPECT_EQ(run("test_sdio dir=both mtu=100 count=1"), "OK\n");
    EXPECT_EQ(test.sdio_req.axi_agent[0], 1u);  // default cache,psram
    EXPECT_EQ(test.sdio_req.axi_agent[1], 7u);
    EXPECT_EQ(run("test_sdio dir=both mtu=100 count=1 axi=off"), "OK\n");
    EXPECT_EQ(test.sdio_req.axi_agent[0], 0u);
    EXPECT_EQ(test.sdio_req.axi_agent[1], 0u);
    EXPECT_EQ(run("test_sdio dir=both mtu=100 count=1 axi=PSRAM"), "OK\n");
    EXPECT_EQ(test.sdio_req.axi_agent[0], 7u);
    EXPECT_EQ(test.sdio_req.axi_agent[1], 0u);
    EXPECT_EQ(run("test_sdio dir=both mtu=100 count=1 axi=h264a,lcd"), "OK\n");
    EXPECT_EQ(test.sdio_req.axi_agent[0], 5u);
    EXPECT_EQ(test.sdio_req.axi_agent[1], 9u);
    const int calls = test.sdio_calls;
    EXPECT_EQ(run("test_sdio dir=both mtu=100 count=1 axi=sdmmc"), "NOK EINVAL\n");
    EXPECT_EQ(run("test_sdio dir=both mtu=100 count=1 axi=cache,psram,lcd"), "NOK EINVAL\n");
    EXPECT_EQ(run("test_sdio dir=both mtu=100 count=1 axi=cache,"), "NOK EINVAL\n");
    EXPECT_EQ(run("test_sdio dir=both mtu=100 count=1 axi="), "NOK EINVAL\n");
    EXPECT_EQ(test.sdio_calls, calls);
}

TEST_F(MplaneCommandsTest, TestSdioRejectsBadRequests)
{
    const int calls = test.sdio_calls;
    EXPECT_EQ(run("test_sdio dir=both mtu=7 count=1"), "NOK EINVAL\n");
    EXPECT_EQ(run("test_sdio dir=both mtu=1505 count=1"), "NOK EINVAL\n");
    EXPECT_EQ(run("test_sdio dir=up mtu=100 count=1"), "NOK EINVAL\n");
    EXPECT_EQ(run("test_sdio mtu=100 count=1"), "NOK EINVAL\n");
    EXPECT_EQ(run("test_sdio dir=both mtu=100 count=1 probe=l2"), "NOK EINVAL\n");
    EXPECT_EQ(run("test_sdio dir=both mtu=100"), "NOK EINVAL\n");
    EXPECT_EQ(run("test_sdio dir=both mtu=100 count=1 rate=5"), "NOK EINVAL\n");
    // Paced runs over an hour (32-bit microsecond counters).
    EXPECT_EQ(run("test_sdio dir=p4c6 mtu=100 count=10000 pacing_us=360001"), "NOK EINVAL\n");
    EXPECT_EQ(test.sdio_calls, calls);
    EXPECT_EQ(run("test_sdio dir=p4c6 mtu=100 count=10000 pacing_us=360000"), "OK\n");
    test.next_status = mplane_status::already;
    EXPECT_EQ(run("test_sdio dir=both mtu=100 count=1"), "NOK EALREADY\n");
}

TEST_F(MplaneCommandsTest, TestSdioStatIdle)
{
    EXPECT_EQ(run("test_sdio_stat"), "test_sdio_stat state=idle\n");
    EXPECT_EQ(run("tsds x=1"), "NOK EINVAL\n");
}

TEST_F(MplaneCommandsTest, TestSdioStatReport)
{
    sdio_test_result& r = test.sdio_result;
    r.state = sdio_test_state::done;
    r.run = 3;
    r.req.dir = sdio_test_dir::p4c6;
    r.req.mtu = 1400;
    r.req.count = 1000;
    r.req.probe = sdio_test_probe::internal;
    r.p4c6.ran = true;
    r.p4c6.sent = 1000;
    r.p4c6.sender_us = 230000;
    r.p4c6.rx = 999;
    r.p4c6.lost = 1;
    r.p4c6.rx_us = 224000;
    r.p4c6.rx_bytes = 1398600;
    r.c6_base.valid = true;
    r.c6_base.window_us = 500000;
    r.c6_base.cores = 1;
    r.c6_base.busy_permille[0] = 21;
    r.c6_base.probe_ok = true;
    r.c6_base.probe_bytes = 50000000;
    r.c6_base.probe_us = 400000;
    r.p4_base.valid = true;
    r.p4_base.window_us = 500100;
    r.p4_base.cores = 2;
    r.p4_base.busy_permille[0] = 30;
    r.p4_base.busy_permille[1] = 0xffff;
    r.p4_base.probe_ok = false;
    r.c6_load = r.c6_base;
    r.c6_load.busy_permille[0] = 873;
    r.c6_load.probe_bytes = 10000000;
    r.p4_load = r.p4_base;
    r.p4_load.axi_agent[0] = 1;
    r.p4_load.axi_agent[1] = 7;
    r.p4_load.axi_rd_mean_permille[0] = 123;
    r.p4_load.axi_wr_mean_permille[0] = 40;
    r.p4_load.axi_rd_max_permille[0] = 551;
    r.p4_load.axi_wr_max_permille[0] = 200;
    r.p4_load.axi_rd_mean_permille[1] = 310;
    r.p4_load.axi_wr_mean_permille[1] = 95;
    r.p4_load.axi_rd_max_permille[1] = 999;
    r.p4_load.axi_wr_max_permille[1] = 402;
    r.p4_load.axi_samples = 2200;
    r.p4_credit_waits = 12;
    r.p4_rx_reads = 40;
    EXPECT_EQ(run("test_sdio_stat"),
              "test_sdio_stat state=done run=3 dir=p4c6 mtu=1400 count=1000 pacing_us=0 probe=int "
              "base_ms=500 axi=cache,psram\n"
              "test_sdio_stat flow=p4c6 sent=1000 send_fail=0 tx_us=230000 rx=999 lost=1 reorder=0 "
              "bad=0 rx_us=224000 rx_bytes=1398600 kbps=49950\n"
              "test_sdio_stat cpu=base c6_us=500000 p4_us=500100 c6=2.1 p4c0=3.0 p4c1=-\n"
              "test_sdio_stat cpu=load c6_us=500000 p4_us=500100 c6=87.3 p4c0=3.0 p4c1=-\n"
              "test_sdio_stat membus=base mem=int c6=125.0 p4=-\n"
              "test_sdio_stat membus=load mem=int c6=25.0 p4=-\n"
              "test_sdio_stat axi=base unavailable\n"
              "test_sdio_stat axi=load cache_rd=12.3 cache_wr=4.0 cache_rd_max=55.1 "
              "cache_wr_max=20.0 psram_rd=31.0 psram_wr=9.5 psram_rd_max=99.9 psram_wr_max=40.2 "
              "samples=2200\n"
              "test_sdio_stat link p4_credit_waits=12 p4_rx_reads=40 p4_tx_writes=0 p4_tx_slots=0 "
              "p4_tx_msgs=0 freq_khz=0 link_downs=0\n");
    r.state = sdio_test_state::failed;
    r.error = "p4_no_reply";
    r.p4c6 = sdio_test_flow{};
    r.req.probe = sdio_test_probe::off;
    r.req.axi_agent[0] = r.req.axi_agent[1] = 0;
    r.c6_base = r.c6_load = r.p4_base = r.p4_load = sdio_test_window_stats{};
    EXPECT_EQ(run("test_sdio_stat"),
              "test_sdio_stat state=failed error=p4_no_reply run=3 dir=p4c6 mtu=1400 count=1000 "
              "pacing_us=0 probe=off base_ms=500 axi=off\n");
}
