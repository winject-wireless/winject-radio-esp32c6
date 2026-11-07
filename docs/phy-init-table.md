# PHY init table and per-rate TX power (C6)

Scope: the 144-byte PHY init blob used by the C6's default build, and how its per-rate values become the MAC's per-rate power table. Read from ESP-IDF v5.5.5 `libphy.a` and `libpp.a` (RISC-V objects, disassembled with `riscv32-esp-elf-objdump`). Measurements are from the rover C6 on 2026-11-04. Nothing here is verified by a reflash.

## 1. Blob layout

| Offset | Size | Field |
|---|---|---|
| 0x00 | 8 | magic `PHYINIT\0` |
| 0x08 | 128 | `params[128]` (IDF `esp_phy_init_data_t`, `components/esp_phy/include/esp_phy_init.h`) |
| 0x88 | 8 | magic `PHYINIT\0` |

Total 144 bytes. The default build uses the copy compiled into the firmware (`CONFIG_ESP_PHY_INIT_DATA_IN_PARTITION` is off). The `phy_init` partition at 0xF000 is used only when that flag is on.

## 2. Blob to `phy_param`

`register_chipv7_phy_init_param` (libphy, `phy_init.o`) copies parts of `params[]` into the global `phy_param` array. Decoded from the disassembly:

| `params[]` index | Copied to `phy_param` offset | Notes |
|---|---|---|
| 0 | 0x50 | not used by the clamp path; value 0x0A in `base.bin` |
| 2..19 | 0x52..0x63 | 18 per-rate targets (the source read by the clamp) |
| 24 | 0x66 | mode selector; 0 in `base.bin`. A value of 1 selects the FCC path (`get_rate_fcc_index`, not analysed) |
| 25..38 | 0x70..0x7D | not analysed |
| 39..52 | 0x7E..0x8B | not analysed |
| 53..66 | 0x8C..0x99 | not analysed |
| 1, 20..23, 67..127 | not copied in the segments above | 67..127 not decoded yet |

## 3. Rate code to slot

`rate_to_index` (libphy, `phy_11ax.o`) maps an IDF rate code (`wifi_phy_rate_t`, `components/esp_wifi/include/esp_wifi_types_generic.h`) to a slot 0..n. Reading of the code:

| Rate code | Rates | Slot |
|---|---|---|
| 0x00, 0x01, 0x05 | 1M_L, 2M_L, 2M_S | 0 |
| 0x02, 0x03, 0x06, 0x07 | 5.5M_L, 11M_L, 5.5M_S, 11M_S | 1 |
| 0x08, 0x09 | 48M, 24M | 6 |
| 0x0A, 0x0B | 12M, 6M | 7 |
| 0x0C, 0x0D | 54M, 36M | 8 |
| 0x0E, 0x0F | 18M, 9M | 9 |
| 0x10..0x17 | MCS0..7 LGI | 6, 6, 7, 7, 8, 8, 9, 9 |
| 0x18 and up | MCS8/9 LGI and the rest | code − 14 (10 and up) |

Open: for codes 0x08 and up, if a hardware bit is set (bit 10 of the word at 0x600A8060), the function returns slot 0 for all rates. That bit is not identified.

## 4. Slot to table value

`hal_init_tx_pwr` (libpp, `hal_mac_tx.o`) calls `get_target_pwr(rate, out)` for each of the 43 rate entries, 2 bytes per entry. `get_target_pwr` calls `rate_to_index`, then reads one byte from a buffer filled by `wifi_get_target_power_new` (libphy, `phy_tx_gain.o`). That function passes:
- the cap `most_tpw` = `phy_param[6]`, which `phy_set_most_tpw` writes (the `esp_wifi_set_max_tx_power` path)
- the source = `phy_param + 0x52`, 18 bytes
- the group limits (below)

`get_chan_target_power_new` (same object) computes, for each i in 0..17:

    out[i] = min( phy_param[0x52 + i], G[group(i)], most_tpw )

Groups and limits:

| i | Group | G (default) |
|---|---|---|
| 0..1 | G0 | 84 |
| 2..5 | G1 | 84 |
| 6..13 | G2 | 84 |
| 14..17 | G3 | 84 |

The four G values come from the first word of `.rodata` in `phy_tx_gain.o`, which is 0x54545454 (four times 84). This is inferred: `.rodata` is the only data section in that object, and the load uses offset 0. The symbol's address was not checked directly. The 84 cap is therefore a compile-time constant, not a value from the blob. `get_target_pwr` then clamps to 84 a second time.

If `phy_param[0x66]` is 1, the FCC path (`get_rate_fcc_index`) replaces the G values. Not analysed.

## 5. Slot to rate and value in `base.bin`

| Slot | Blob index | Value | dBm | Rates (from section 3) |
|---|---|---|---|---|
| 0 | 2 | 0x50 | 20 | 1M_L, 2M_L, 2M_S |
| 1 | 3 | 0x50 | 20 | 5.5M_L, 11M_L, 5.5M_S, 11M_S |
| 2..4 | 4..6 | 0x50 | 20 | not reached by the rate codes above |
| 5 | 7 | 0x4C | 19 | not reached by the rate codes above |
| 6 | 8 | 0x4C | 19 | 48M, 24M, MCS0/1 LGI |
| 7 | 9 | 0x4C | 19 | 12M, 6M, MCS2/3 LGI |
| 8 | 10 | 0x4C | 19 | 54M, 36M, MCS4/5 LGI |
| 9 | 11 | 0x48 | 18 | 18M, 9M, MCS6/7 LGI |
| 10 | 12 | 0x44 | 17 | MCS8 LGI |
| 11 | 13 | 0x3C | 15 | MCS9 LGI |
| 12..17 | 14..19 | 0x3C, 0x3C, 0x3C, 0x4C, 0x4C, 0x48 | 15, 15, 15, 19, 19, 18 | codes 0x1A..0x1F (by the code − 14 rule; not checked against the enum) |

Slots 2..5 and 12..17 need checking against the full enum and any multi-rate path.

## 6. The 85..88 boost

`hal_set_tx_pwr(index, q)` writes the libpp table directly. It doesn't go through `get_target_pwr` or the G clamp. That is why values above 84 work on the boost path. `phy_set_most_tpw` raises the cap for the same path.

Firmware boost (`c6/main/radio/wifi_tx.cpp`, `boost_rate`, `apply_power`): the boost sets the table entries for rate codes 0x00–0x03, 0x05–0x07, 0x09–0x0B and 0x0D–0x0F to the requested level. These are DSSS, CCK and legacy OFDM up to 36 Mbps. 48M (0x08) and 54M (0x0C) keep their table value. 36M is copied from 24M after each table rebuild, so the two rates stay identical.

## 7. Measured effect (rover C6, OFDM 24M, 2026-11-04)

| Variant | Entries changed | Cap | Output |
|---|---|---|---|
| base | none (values in section 5) | 84 | +13.5 dBm |
| low84 | blob 2..10 and 16..18 (0x4C and 0x50) to 0x54 | 84 | +14.55 dBm |
| all84 | all 18 to 0x54 | 84 | +14.5 dBm |
| all 0x60 | all 18 to 0x60 | 84 | +14.75 dBm (read back 84) |
| boost to 88 | none in the table | 88 | about +0.7 dB above cap 84 (one run, 30 dB direct) |

`low88` (blob 2..10 and 16..18 at 0x58) was not measured at cap 84. Its only measurement was through the 13 dB PA, with the boost, which is not comparable.

The target value does not set the radiated power directly. With target 19 dBm the output was 13.5 dBm, about 5.5 dB lower. This gap is not explained.

## 8. Open items

1. The bit that makes `rate_to_index` return slot 0 for all rates ≥ 8 (word at 0x600A8060, bit 10) is unidentified.
2. Slots above 17 can index past the 18-byte buffer. Confirm whether those rates are reachable.
3. `phy_param[0x66] == 1` (FCC path) is not analysed.
4. `params[67..127]` and `params[0]`, `params[127]` are not decoded. `params[1]` is not copied.
5. EVM for high-order rates is not measured. Raising entries for MCS5..7 or 54M can break EVM limits in the datasheet (54M −25 dB, MCS7 −27 dB, MCS9 −32 dB).
6. The output-to-target relation (section 7) is unexplained.

## 9. How to reproduce

    IDF=$HOME/esp/esp-idf-v5.5.5
    LIB=$IDF/components/esp_phy/lib/esp32c6/libphy.a
    NM=$HOME/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20260121/riscv32-esp-elf/bin/riscv32-esp-elf-nm
    OBJ=$HOME/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20260121/riscv32-esp-elf/bin/riscv32-esp-elf-objdump
    $NM -A $LIB | grep " T get_chan_target_power_new"        # find the member
    ar x $LIB phy_tx_gain.o
    $OBJ -dr -j .text.get_chan_target_power_new phy_tx_gain.o  # relocations show callees
    $OBJ -s -j .rodata phy_tx_gain.o                           # G limits (0x54545454)

Member names used here: `phy_11ax.o` (`get_target_pwr`, `rate_to_index`), `phy_tx_gain.o` (`wifi_get_target_power_new`, `get_chan_target_power_new`), `phy_init.o` (`register_chipv7_phy_init_param`), `hal_mac_tx.o` in `libpp.a` (`hal_init_tx_pwr`).
