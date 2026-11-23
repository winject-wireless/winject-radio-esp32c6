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

- **`len`** is needed because one SDIO transfer carries several messages in both directions. A receiver walks the transfer message by message; `len = 0` with `plane = 0` ends a transfer early (the rest is padding).
- **P4 → C6 framing:** the P4 writes in whole C6 receive buffers (1536 bytes, `SDIO_LINK_SLAVE_RX_BUF_SIZE`). Each buffer holds whole messages, never part of one, and one with room left ends with a `plane = 0, len = 0` header. The C6 parses every receive buffer on its own, also when one host write fills several (`ESP_ERR_NOT_FINISHED`).
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

## Test plane (`plane = 4`)

Raw link capacity, CPU and memory-bus load, measured by the production firmware. The C6 drives it from the m-plane; the P4 proxy only answers. Messages and fields: [`components/sdio_msg/include/sdio_test.h`](../components/sdio_msg/include/sdio_test.h).

| Message | Direction | Meaning |
|---------|-----------|---------|
| `bw_tx` (mtu, count, pacing_us) | C6 → P4 | The P4 sends `count` test packets of `mtu` bytes, one every `pacing_us` (0 = as fast as the link takes them); `count` 0 stops it |
| `bw_tx_done` | P4 → C6 | After its last packet: sent, send failures, elapsed time |
| `test_packet` (run, seq, filler) | both | Counted by the receiver per run: packets, bytes, sequence gaps, reorders, first-to-last time |
| `get_rx_stats` (clear, probe, axi ports) | C6 → P4 | The P4 replies `rx_stats` for its current window; with `clear` it starts a new one (counters, CPU, probe, AXI) and sets its memory probe and AXI monitor ports |
| `rx_stats` | P4 → C6 | Received test packets, CPU per core, probe figures and link counters for the window (printed as `p4_credit_waits p4_rx_reads p4_tx_writes p4_tx_slots p4_tx_msgs freq_khz link_downs`); echoes the request's tag so a late reply is not taken for the next one. Later fields are appended: a C6 accepts a shorter reply from an older P4 (missing fields read 0) and ignores fields it does not know |

A P4 firmware without the test plane drops the rest of a transfer that carries one, so update both sides together.

```
test_sdio dir=<p4c6|c6p4|both> mtu=<8-1504> count=<1-1000000, count × pacing_us ≤ 1 h> [pacing_us=<us>] [probe=<off|int|psram>] [base_ms=<ms, default 500>] [axi=<off|port[,port]>, default cache,psram]
test_sdio count=0          # stop
test_sdio_stat             # state=running|done|stopped|failed and the result
```

`test_sdio` returns at once; the run goes on in the background:

1. Both chips start their memory probe and open a window (the P4 on `get_rx_stats clear`).
2. `base_ms` with no test traffic: the baseline CPU and probe figures.
3. Traffic: `bw_tx` to the P4 (`p4c6`) and/or the C6's own sender (`c6p4`), both at once with `dir=both`.
4. Load window read on both chips; the P4's reply also carries its count of the C6's packets. Probes off.

Example result (the format; the numbers are illustrative, not a measurement):

```
test_sdio_stat state=done run=3 dir=p4c6 mtu=1400 count=1000 pacing_us=0 probe=int base_ms=500 axi=cache,psram
test_sdio_stat flow=p4c6 sent=1000 send_fail=0 tx_us=230000 rx=999 lost=1 reorder=0 bad=0 rx_us=224000 rx_bytes=1398600 kbps=49950
test_sdio_stat cpu=base c6_us=500000 p4_us=500100 c6=2.1 p4c0=3.0 p4c1=1.2
test_sdio_stat cpu=load c6_us=500000 p4_us=500100 c6=87.3 p4c0=20.4 p4c1=9.8
test_sdio_stat membus=base mem=int c6=125.0 p4=210.3
test_sdio_stat membus=load mem=int c6=25.0 p4=190.6
test_sdio_stat axi=base cache_rd=4.1 cache_wr=1.0 cache_rd_max=30.2 cache_wr_max=9.9 psram_rd=10.2 psram_wr=3.1 psram_rd_max=61.0 psram_wr_max=22.4 samples=500
test_sdio_stat axi=load cache_rd=12.3 cache_wr=4.0 cache_rd_max=55.1 cache_wr_max=20.0 psram_rd=31.0 psram_wr=9.5 psram_rd_max=99.9 psram_wr_max=40.2 samples=2200
test_sdio_stat link p4_credit_waits=12 p4_rx_reads=40 p4_tx_writes=260 p4_tx_slots=1000 p4_tx_msgs=1004 freq_khz=40000 link_downs=0
```

- **`lost` / `reorder`**: sequence gaps, and packets arriving below the highest seq so far (late or repeated). A late packet lowers `lost` only if its seq was a gap within the last 256.
- **`kbps`**: test-packet bytes (the whole d-plane-sized payload, without the 4-byte header and padding) over the receiver's first-to-last time.
- **`cpu`**: percent of each core not spent in the idle task or the memory probe, over the window (`-` without FreeRTOS run-time stats; the P4 needs `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS` and `CONFIG_FREERTOS_USE_TRACE_FACILITY`). Other traffic on the link (video, m-plane) is included.
- **`membus`**: the memory probe, one memcpy task per core at idle priority, reported as MB copied per second of its own CPU time (all cores together on the P4). The drop from `base` to `load` is the contention the link adds to that memory. `int`: the C6's SRAM (2 × 8 KB) and, on the P4, 2 × 8 KB of L2MEM per core through its uncached alias, so every access goes to the bus. `psram`: the P4's PSRAM only (2 × 256 KB per core, cached; the C6 has none). The probe uses the idle time, so `cpu` subtracts it and the link figures can move a little with it on: compare runs with the same `probe`.
- **`axi`** (optional; P4 chip revision 3.0+ only, e.g. the Waveshare rover board): the P4's AXI performance monitor (TRM chapter 24) on up to two of its ports, `cache vdma1 vdma2 dma2d h264a h264b psram cam lcd`. Each figure is data beats per bus cycle in percent, i.e. the share of that port's read or write data channel in use: the mean (the monitor's last four 50 000-cycle windows, sampled every millisecond) and the busiest window. The SDMMC host is not one of the monitored ports, so this shows how loaded the CPU cache and PSRAM ports are while the link runs, not the SDIO DMA itself. A P4 below rev 3.0 (the Guition ground board is v1.3), or `axi=off`, prints `axi=… unavailable` and the rest of the test runs as usual. IDF has no driver for the monitor; the firmware programs its registers from the TRM, and the unit-to-port field is taken as 5 bits per unit as the TRM states (the IDF register header comment says 4).

## Bring-up

The P4 resets the C6 (CHIP_PU) and enumerates the SDIO card. Enumeration can succeed against the C6 ROM's slave before the firmware re-initialises it, so the C6 writes `0xC6` to slave scratch register 0 once its link is up, and the P4 repeats enumeration until it reads that value.

## Flow control

The SDIO slave only accepts a host write when it has receive buffers loaded, so the link itself never drops. On the P4, `sdio_link_host_send()` queues the message (ring of 8 buffer-sized slots) and returns; a sender waits only while the queue is full, up to its timeout (`tx_errors`). A message still queued when the link goes down is dropped and counted the same way. Drops stay where the counters already account for them: the C6's inject queue (`dropped_tx_queue`) and RX pool (`dropped_rx_queue`).

## P4 proxy

The proxy binds the radio's UDP ports on the P4's lwIP and forwards datagrams unchanged:

| UDP | → SDIO | SDIO → | UDP destination |
|-----|--------|--------|-----------------|
| 22 (m-plane) | each request datagram → one m-plane message | each m-plane message → one datagram | source of the most recent m-plane request |
| 9000 (d-plane) | each datagram (inject or registration) → one d-plane message | each d-plane message → one datagram | source of the most recent registration, the same rule as the radio |

Replies go to the last requester, so one m-plane client at a time is expected (winject-manager).

## Bus

4-bit SDIO at 40 MHz in high-speed mode (160 Mbit/s raw). If high-speed mode errors (3 bus errors in a bring-up, or 3 link losses within 60 s), the P4 drops to 20 MHz until it restarts (`freq_fallbacks` in the link stats; `test_sdio_stat` prints `freq_khz`). The C6 sends in stream mode, so one host read collects every message it has queued (up to 8 KB). The P4's TX task writes every queued slot it can in one block-mode CMD53 (up to 8 slots, as many as the C6 has free receive buffers; `tx_writes` / `tx_slots` in the link counters). While the TX task waits for the bus, the host reads about one buffer at a time so the two directions alternate.

Before 2026-11-23 the P4 wrote each message by itself (a 1024-byte block write plus a byte-mode tail for a 1400-byte message), which is why P4 → C6 was slower than C6 → P4 in the measurements below (rover, `test_sdio`, 1400-byte packets: 45.9 vs 55.9 Mbit/s, 25.0 + 39.6 both ways). Batched writes at 20 MHz: 61.2 vs 56.6, 37.7 + 29.7. At 40 MHz: 84.4 vs 75.8, 50.0 + 39.8 both ways (42 s, no loss, no link losses).

Measured with `sdio_link_host` / `sdio_link_slave` (1400-byte d-plane payloads, no loss in any step): 50 Mbps P4 → C6, 62 Mbps C6 → P4, about 56 Mbps total when both saturate (split about 13 / 43 in favour of C6 → P4). The C6 spends about 3% CPU per 1000 messages/s.

Injecting frames that arrive over the link reaches the channel limit at OFDM_54M (3000 frames/s, 33.6 Mbit/s, with 0.4% inject-queue drops). An earlier reading that SDIO traffic slows the C6's WiFi transmit came from test frames filled with a constant byte (see [open-issues.md](open-issues.md), issue 1).
