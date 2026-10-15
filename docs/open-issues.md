# Open issues

Found while bringing up the C6 radio end to end (2026-10-15). Numbers are from
the Waveshare ESP32-P4-WIFI6-DEV-KIT bench, ch 1, 1400-byte frames.

## 1. Repeating payloads slow the C6's transmit (resolved: test artifact)

The reported "SDIO slows WiFi TX" was the frame body. Bench frames were filled
with a constant 0xa5 (rtk_loss, c6_radio and the first end-to-end bench). With
that fill, the C6 defers on CCA far more often and tops out near 2100 frames/s
at OFDM_54M; `cca=false` restores full rate. With varied bytes (as real,
FEC-coded payloads are), frames injected over SDIO reach the channel limit:
3000 frames/s (33.6 Mbit/s) offered, 122 of 30,000 dropped at the inject queue,
the same rate the built-in generator reaches with the link idle (about
3200 frames/s). SDIO traffic alone (2000 frames/s crossing the link and
discarded) does not slow the generator either. Body test, 2500 frames/s at
OFDM_54M, CCA on:

| Body | On air | Inject-queue drops |
|---|---|---|
| all zeros | full rate | 0 |
| varying bytes | full rate | 0 |
| constant 0xa5 | ~1957 frames/s | 2672 of 15,000 |

Open: why a constant 0xa5 body (but not zeros) makes CCA defer. The step-1
link bench and the eppp comparison used 0xa5 frames, so their TX-slowdown
numbers are confounded by this. Diagnostic: the C6 UART `mon 1` prints the
inject path's rates, queue, in-flight and per-task CPU once a second.

## 2. Rate changes restarted the WiFi driver (resolved)

On the C6, `esp_wifi_config_80211_tx_rate` returns `ESP_OK` while the driver
runs but leaves the rate unchanged (frames stayed at 1 Mbit/s). The first
workaround restarted the driver for every modulation change (about 31 ms, frames
in flight dropped, promiscuous receive briefly off). Testing each call while
running showed the restart is not needed: `esp_wifi_config_80211_tx` (phy mode
+ rate) applies immediately, the old call never does, and the protocol mask was
already b/g/n. `apply_modulation` now uses `esp_wifi_config_80211_tx` without a
restart, and `initialize` sets the protocol before `esp_wifi_start` so the
driver never starts with 11ax. Checked on air (8811AU-decoded rate and frames/s)
for OFDM_54M, OFDM_24M, OFDM_MCS7_LGI and DSS_1M_L; `radio_tx` takes 3.6 ms
round trip through the P4, `dropped_wifi` stays 0.

## 3. No firmware update through the P4

The C6 can only be reflashed over its own UART (`dl`, then esptool); the P4 app
has no OTA either. A deployed unit needs an update path: e.g. an OTA plane on
the SDIO link for the C6 and HTTP OTA on the P4.

## 4. Network settings

`network ip=… type=…` and `test_ether_*` reply `NOK ENOTSUP` on the C6. The P4
uses DHCP only and its address cannot be set over the m-plane; there is no
static fallback like the Ethernet radio's 192.168.32.1.

## 5. Proxy addressing

- m-plane replies go to the source of the most recent request, so only one
  m-plane client at a time.
- Any 1–23 byte datagram on UDP 9000 re-targets where received frames are sent.

Both match the Ethernet radio's behaviour, but the proxy also accepts them from
anywhere on the LAN.

## 6. Code copied from winject-radio-esp32

`c6/main` carries the radio, m-plane and settings code of winject-radio-esp32
at 8c1cb3a, with C6 changes (rate API, A-MPDU, cores, LEDs). Fixes in either
repo do not reach the other. Options: a shared component/submodule, or
porting fixes by hand with the carried-over host tests (`test/winject_host`).

## 7. TX pacing constants not re-tuned

The in-flight cap (4), burst size (8) and burst gap (1 ms) come from the
Ethernet radio, where they protect EMAC DMA. They do not limit rate on the C6
(3019 frames/s at 54M from the test generator) but were not tuned for SDIO.

## 8. Bench peer

The ESP32 bench radios (192.168.253.11/.12) were offline, so the end-to-end
runs used the RTL8811AU through winject-radio-realtek with
`radio.driver = rtl88xxau_winject` (its configs expect `rtl88xxau_wfb`). That
manager logged a few console ping timeouts to the Realtek radio. The C6 has not
been run against an ESP32 radio or a second C6.

## 9. No A-MPDU filtering on the C6

The C6's `rx_ctrl` (WiFi 6 MAC) has no `aggregation` / `ampdu_cnt`, so
multi-subframe A-MPDUs are forwarded instead of dropped. winject never sends
A-MPDUs, so only foreign traffic is affected.

## 10. An m-plane `reset` leaves the C6 unreachable

`reset` reboots the C6 alone. The P4 does not notice and never re-initialises
the SDIO link, so the radio stays unreachable until the P4 restarts (which
resets the C6 again). The P4 needs to detect the slave reset (link errors, or
the ready register clearing) and redo the bring-up; settings saved before the
reset (e.g. `tune_*` + `save`) do take effect once the link is back.

