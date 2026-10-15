# winject-radio-esp32c6

**winject radio** firmware for the **ESP32-C6** as the radio coprocessor of an **ESP32-P4** board (first target: Waveshare ESP32-P4-WIFI6-DEV-KIT, C6 on SDIO). The C6 does raw 802.11 inject and promiscuous receive; the m-plane and d-plane run over SDIO to the P4 instead of UDP over Ethernet. Built with native **ESP-IDF v5.5**.

The m-plane commands and the d-plane datagrams are the same as in [winject-radio-esp32](https://github.com/winject-wireless/winject-radio-esp32) ([m-plane](https://github.com/winject-wireless/winject-radio-esp32/blob/main/docs/mplane.md), [radio architecture](https://github.com/winject-wireless/winject-radio-esp32/blob/main/docs/winject.md)). SDIO adds only a 4-byte header that says which plane a message belongs to and how long it is: [docs/sdio-link.md](docs/sdio-link.md).

```
 winject-manager ──UDP 22 / 9000──▶ P4 proxy ──SDIO (plane + len header)──▶ C6: m-plane, wifi_tx ─▶ air
 (winject-l3)    ◀─────────────────  (lwIP)  ◀──────────────────────────────  C6: wifi_rx         ◀─ air
```

On the P4, a proxy ([winject-radio-esp32-sdio-proxy](https://github.com/winject-wireless/winject-radio-esp32-sdio-proxy)) exposes the radio's usual UDP ports (m-plane 22, d-plane 9000), so **winject-manager** talks to it exactly as it does to an Ethernet radio. This repo holds the C6 firmware and the SDIO wire format (`sdio_msg`), which the proxy fetches from here.

The C6 runs no IP stack: lwIP over SDIO cost about 13–14% of its single 160 MHz core per 1000 packets/s on the bench, the raw SDIO link about 3%.

## Status

Working end to end on the bench (2026-10-15): winject-manager drives the C6 radio through the P4 at 192.168.253.x like an Ethernet radio (version check, `radio_caps_info`, `radio_tx`, domain filter, `save`), with iperf traffic both ways against an RTL8811AU radio (`winject-radio-realtek`):

| iperf UDP, 1400 B, OFDM_24M, ch 1 | C6 injects (A→B) | C6 receives (B→A) |
|---|---|---|
| 5 Mbit/s | 0.55% loss | 0.18% |
| 10 Mbit/s | 0.44% (3.2% in another run) | 0.12–0.15% |
| 15 Mbit/s | 0.88% | 0.20% |

In the 10 + 15 Mbit/s run the C6 dropped 1 of 28,095 inject frames (`dropped_tx_queue`) and forwarded every received frame; the rest of the loss is on air.

Open issues are tracked in [docs/open-issues.md](docs/open-issues.md). Known limits (see [docs/sdio-link.md](docs/sdio-link.md#bus)):
- Inject over SDIO reaches the channel limit at OFDM_54M: 3000 frames/s (33.6 Mbit/s) with 0.4% inject-queue drops and 0.7% air loss to the 8811AU.
- The P4 re-establishes the link when the C6 reboots (m-plane `reset`, crash); a silent C6 is reset only after 30 s.
- Firmware update over HTTP on the P4: `POST /update/p4` works; `POST /update/c6` (over SDIO) is implemented but not yet tested on hardware (issue 3).

## Layout

```
docs/sdio-link.md                 SDIO message format, bring-up, P4 proxy behaviour, measurements
components/sdio_msg/              Message encode/parse, shared by both chips (no IDF dependencies)
components/sdio_link_slave/       C6: SDIO slave link
c6/                               C6 radio firmware (radio, m-plane, settings from winject-radio-esp32)
test/host/                        Host unit tests for sdio_msg
test/winject_host/                Host unit tests carried over from winject-radio-esp32 (GoogleTest)
```

## Build and flash

Native ESP-IDF v5.5 (`. ~/esp/esp-idf-v5.5.5/export.sh`). On the Waveshare kit the P4's USB-UART is the Type-C "UART" port; the C6's UART0 is on the P2 header (a 3.3 V USB-UART adapter).

```bash
# C6: enter the ROM downloader with `curl -X POST http://<P4 IP>/c6/download` (the P4
# holds the C6's GPIO9 strap low; works with no bootable image) or by sending "dl" on
# its UART (running firmware only), then
cd c6 && idf.py build && cd build && \
  python -m esptool --chip esp32c6 -p /dev/ttyACM1 --before no_reset --after no_reset write_flash @flash_args
# and boot it: curl -X POST http://<P4 IP>/c6/reset
```

The P4 side (proxy image, or an app that includes it such as rover-remote-esp32p4) is built from [winject-radio-esp32-sdio-proxy](https://github.com/winject-wireless/winject-radio-esp32-sdio-proxy).

After that, update both over HTTP through the P4 (raw image as the body); each keeps its previous image until the new one proves itself:

```bash
curl --data-binary @c6/build/winject-esp32c6.bin http://<P4 IP>/update/c6   # C6: kept once it answers the P4
curl --data-binary @winject-p4-proxy.bin http://<P4 IP>/update/p4           # P4: kept once reached (e.g. curl http://<P4 IP>/)
```

Flash the C6 over its UART only while the P4 runs a firmware with the 30 s quiet window (any build from 71eb94a on): older ones reset a silent C6 mid-flash.

The P4 resets the C6 at boot, gets an address by DHCP (hostname `winject-esp32c6`, printed as `WINJECT_P4_IP ip=…`) and serves the radio on UDP 22 / 9000 of that address. Point winject-manager at it like an ESP32 radio (`winject.device = <P4 IP>`, `console = 22`, `dplane_port = 9000`, `radio_fcs = signal`). The P4 prints proxy and link counters every 10 s (`WINJECT_P4_STATS`).

## Differences from winject-radio-esp32

| Topic | C6 radio |
|---|---|
| Host link | SDIO to the P4 (20 MHz, 4-bit); the P4 proxy owns the IP address and UDP ports |
| Version | `v1.0.x`: same manager ↔ radio protocol (1.0) |
| `network` | Set replies `NOK ENOTSUP`; the stored values are still carried in slots |
| `test_ether_*` | `NOK ENOTSUP` |
| Firmware update | HTTP on the P4 (`POST /update/c6`, `/update/p4`), two slots + rollback on both chips; `POST /c6/download` (P4 holds GPIO9 low) or UART `dl`, then esptool, for the first flash. `reset mode=OTA` boots with the radio off |
| Cores | One core: WiFi driver (23) > `wifi_tx` (20) > SDIO receive (19) > d-plane forward (17) > m-plane (5) |
| Rate setting | The C6 driver ignores `esp_wifi_config_80211_tx_rate` (returns OK, rate unchanged); the firmware uses `esp_wifi_config_80211_tx` (phy mode + rate), which applies while running |
| A-MPDU | The C6's rx_ctrl has no A-MPDU fields, so multi-subframe A-MPDUs are not dropped (winject never sends them) |
| LEDs | None (the WT32 LED pins are the C6's UART) |
| TX pacing | The Ethernet radio's constants (in-flight cap 4, bursts of 8): they do not limit rate here (3000+ frames/s at OFDM_54M) |
| Diagnostics | UART `mon 1`: inject-path rates (`in`, `air`, `submit`, `nomem`, `slotwait`, `dropq`), queue, in-flight, heap, per-task CPU once a second |

## Host unit tests

```bash
cmake -S test/host -B build_host_test && cmake --build build_host_test && ctest --test-dir build_host_test
cmake -S test/winject_host -B build_winject_host_test && cmake --build build_winject_host_test && ctest --test-dir build_winject_host_test
```
