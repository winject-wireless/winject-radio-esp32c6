# SDIO link

The P4 (SDIO host) and the C6 (SDIO slave) exchange **messages**. Each message carries exactly one m-plane or d-plane datagram, unchanged from the UDP protocol of [winject-radio-esp32](https://github.com/winject-wireless/winject-radio-esp32). The only addition is a 4-byte header.

## Message

```
offset  size  field
0       1     plane     1 = m-plane, 2 = d-plane
1       1     reserved  0
2       2     len       payload length in bytes, little-endian
4       len   payload   the datagram, byte for byte
        0-3   padding   zeros up to a multiple of 4
```

- **`len`** is needed because one SDIO transfer can carry several messages (the P4 reads everything the C6 has queued at once). A receiver walks the transfer message by message; `len = 0` with `plane = 0` ends a transfer early (the rest is padding).
- **No version field.** The P4 and C6 firmware are built and flashed together, as winject managers and radios are matched today.
- **Unknown `plane`** or a `len` that runs past the transfer: the receiver drops the rest of the transfer and counts it.
- Messages of one plane arrive in the order they were sent.

## m-plane (`plane = 1`)

| Direction | Payload | Size |
|-----------|---------|------|
| P4 → C6 | One request datagram: one or more `cmd:<u8> …` lines | ≤ 1499 bytes, as over UDP |
| C6 → P4 | One reply datagram | ≤ 1500 bytes; longer replies are split at line boundaries into several messages, as UDP replies continue in further datagrams |

Commands, replies and `cmd:<u8>` correlation are unchanged. Setting the Ethernet network (`network ip=… type=…`) and the Ethernet tests (`test_ether_*`) reply `NOK ENOTSUP`: the C6 has no network interface. `network` without arguments still prints the stored values, which are carried in settings slots unchanged.

## d-plane (`plane = 2`)

| Direction | Payload | Meaning |
|-----------|---------|---------|
| P4 → C6 | 24–1472 bytes | Raw MPDU to inject |
| P4 → C6 | 1–23 bytes | Peer registration |
| C6 → P4 | MPDU + 4-byte trailer | Received frame; trailer `00000000` = FCS OK, `FFFFFFFF` = FCS fail (`fcs=SIGNAL`) |

The C6 classifies d-plane payloads by length exactly as the Ethernet radio classifies UDP 9000 datagrams. A registration marks the peer (the P4) present; without one, received frames count as `dropped_no_peer`, as before.

## Bring-up

The P4 resets the C6 (CHIP_PU) and enumerates the SDIO card. Enumeration can succeed against the C6 ROM's slave before the firmware re-initialises it, so the C6 writes `0xC6` to slave scratch register 0 once its link is up, and the P4 repeats enumeration until it reads that value.

## Flow control

The SDIO slave only accepts a host write when it has receive buffers loaded, so the link itself never drops. Drops stay where the counters already account for them: the C6's inject queue (`dropped_tx_queue`) and RX pool (`dropped_rx_queue`).

## P4 proxy

The proxy binds the radio's UDP ports on the P4's lwIP and forwards datagrams unchanged:

| UDP | → SDIO | SDIO → | UDP destination |
|-----|--------|--------|-----------------|
| 22 (m-plane) | each request datagram → one m-plane message | each m-plane message → one datagram | source of the most recent m-plane request |
| 9000 (d-plane) | each datagram (inject or registration) → one d-plane message | each d-plane message → one datagram | source of the most recent registration, the same rule as the radio |

Replies go to the last requester, so one m-plane client at a time is expected (winject-manager).

## Bus

4-bit SDIO at 20 MHz. The C6 sends in stream mode, so one host read collects every message it has queued (up to 8 KB); while a P4 sender is waiting, the host reads about one message at a time so the two directions alternate.

Measured with `sdio_link_host` / `sdio_link_slave` (1400-byte d-plane payloads, no loss in any step): 50 Mbps P4 → C6, 62 Mbps C6 → P4, about 56 Mbps total when both saturate (split about 13 / 43 in favour of C6 → P4). The C6 spends about 3% CPU per 1000 messages/s.

Injecting frames that arrive over the link reaches the channel limit at OFDM_54M (3000 frames/s, 33.6 Mbit/s, with 0.4% inject-queue drops). An earlier reading that SDIO traffic slows the C6's WiFi transmit came from test frames filled with a constant byte (see [open-issues.md](open-issues.md), issue 1).
