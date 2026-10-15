# Open issues

Found while bringing up the C6 radio end to end (2026-10-15). Numbers are from
the Waveshare ESP32-P4-WIFI6-DEV-KIT bench, ch 1, 1400-byte frames.

## 1. SDIO traffic slows the C6's WiFi transmit

With more than about 2000 frames/s arriving from the P4, the C6 sends fewer
frames than it is given: at OFDM_54M, 2500 frames/s in gives about 1775 frames/s
on air, while the radio's own test generator reaches 3019 frames/s with the
link idle. The driver's TX queue is full (`NO_MEM`, all frames in flight) and
the CPU is mostly idle. In a winject radio every injected frame crosses SDIO,
so this caps inject at roughly 20 Mbit/s at 54M.

eppp (lwIP over SDIO) carried 40 Mbit/s into the C6 without slowing TX; the raw
link slows it from about 22–33 Mbit/s. 4 slave receive buffers are better than
16. Cause unknown.

## 2. Rate changes restart the WiFi driver

On the C6, `esp_wifi_config_80211_tx_rate` returns `ESP_OK` while the driver
runs, but frames keep going out at 1 Mbit/s. `wifi::apply_modulation` now stops
and restarts the driver and sets the rate with `esp_wifi_config_80211_tx`. A
modulation change takes about 31 ms, drops what is in flight (counted in
`dropped_wifi`) and briefly stops promiscuous receive.

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
