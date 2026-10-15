# winject-radio-esp32c6

**winject radio** firmware for the **ESP32-C6** as the radio coprocessor of an **ESP32-P4** board (first target: Waveshare ESP32-P4-WIFI6-DEV-KIT, C6 on SDIO). The C6 does raw 802.11 inject and promiscuous receive; the m-plane and d-plane run over SDIO to the P4 instead of UDP over Ethernet. Built with native **ESP-IDF v5.5**.

The m-plane commands and the d-plane datagrams are the same as in [winject-radio-esp32](https://github.com/winject-wireless/winject-radio-esp32) ([m-plane](https://github.com/winject-wireless/winject-radio-esp32/blob/main/docs/mplane.md), [radio architecture](https://github.com/winject-wireless/winject-radio-esp32/blob/main/docs/winject.md)). SDIO adds only a 4-byte header that says which plane a message belongs to and how long it is: [docs/sdio-link.md](docs/sdio-link.md).

```
 winject-manager ──UDP 22 / 9000──▶ P4 proxy ──SDIO (plane + len header)──▶ C6: m-plane, wifi_tx ─▶ air
 (winject-l3)    ◀─────────────────  (lwIP)  ◀──────────────────────────────  C6: wifi_rx         ◀─ air
```

On the P4, a proxy exposes the radio's usual UDP ports (m-plane 22, d-plane 9000), so **winject-manager** talks to it exactly as it does to an Ethernet radio.

The C6 runs no IP stack. A bench on the same board measured lwIP over SDIO at about 13–14% of the C6's CPU per 1000 packets/s. A raw SDIO path costs far less, and the C6 has one 160 MHz core shared with the WiFi driver.

## Status

The SDIO link is implemented and benched (see [docs/sdio-link.md](docs/sdio-link.md#bus)). The radio firmware and the P4 proxy are next.

## Layout

```
docs/sdio-link.md                 SDIO message format, bring-up, P4 proxy behaviour, measurements
components/sdio_msg/              Message encode/parse, shared by both chips (no IDF dependencies)
components/sdio_link_slave/       C6: SDIO slave link
components/sdio_link_host/        P4: SDIO host link (resets the C6, receive task, send)
test/host/                        Host unit tests
```

Planned: `c6/` (radio firmware) and a P4 proxy component.

## Host unit tests

```bash
cmake -S test/host -B build_host_test && cmake --build build_host_test && ctest --test-dir build_host_test
```
