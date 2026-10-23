# The receiver without Wi-Fi and without malloc after boot

The ESP32-S3 receiver no longer starts Wi-Fi. It powers the PHY and calibrates it once at
boot, as a Bluetooth-only application would, and no Wi-Fi code is linked. Each run now takes its
memory from regions reserved when the firmware links, and gives it back at the next run.
ESP-IDF's heap hooks counted every heap call after boot through 5 receive runs, a dump and
membench: there were none. Free heap stayed at 51,482 bytes before, during and after every
run. Decoding is unchanged:
- **8PSK 8/9, 5 Msym/s, GCS:** 102,983 frames exact in 3 runs.
- **8PSK 8/9, 5 Msym/s, TS:** 34,325 frames and 322,983 packets exact.
- **QPSK 3/4, 1 Msym/s:** 2,800 frames exact.

Medians of pilot MER are 29.8 to 29.9 dB at 5 Msym/s and 30.2 dB at 1 Msym/s.

Path: transmitter S3 (44:B1:76:B0:E5:54, c3a1c2d) → coax and 30 dB attenuation → receiver S3
(44:B1:76:B0:DA:C8, f77ef5f), 2420 MHz, short frames. The commands are as in
[the 5 Msym/s runs](../2026-10-23-rx-5m-margin/README.md) and
[the TS runs](../2026-10-23-ts-link/README.md). `runs.jsonl` holds, in order:
- `info` after a reset;
- each receiver summary with its transmitter records;
- `membench` and `dump`;
- `info` again.

## Memory

| Receiver | Before: Wi-Fi per run (c3a1c2d) | Without Wi-Fi (1216b6e) | Arenas (f77ef5f) |
|---|---:|---:|---:|
| Image | 853,872 B | 446,736 B | 432,416 B |
| Static internal RAM | 135,351 B | 116,167 B | 146,311 B, of which 30,720 arena, plus 28,672 B reserved above the banks |
| Heap free before a run | 90,711 B | 110,135 B | 51,482 B |
| Heap free during an 8PSK 8/9 run | 34,223 B | 53,779 B | 51,482 B |
| Largest free block during a run | 8,704 B | 31,744 B | 30,720 B |
| Heap low since boot | 11,316 B (6.1 to 25.0 KB in earlier results) | 44,120 B | 17,836 B, reached during boot |
| Heap calls after boot | every run: 13 to 15 buffers, 5 queues and semaphores, the capture timer, Wi-Fi's own | every run: the same less Wi-Fi's | 0 (counted) |

The before column is from [the TS results](../2026-10-23-ts-link/runs.jsonl) (its GCS 8PSK 8/9
run) and the build's size report. The middle column is from runs of the same code before it was
committed, not recorded here.

**Arenas** (`firmware/rx/main/arena.c`). The most any mode used in these runs is shown below;
`info` confirms at boot that all 12 modes fit.

| Arena | Contents | Used / size |
|---|---|---:|
| Low: static internal RAM below the capture banks | demodulator, clean-path FEC, TS deframer | 28,144 of 30,720 B |
| High: internal RAM from 0x3fce0000, reserved like the banks | symbol ring, three frame slots | 28,576 of 28,672 B |
| PSRAM: static | LDPC decoder state and work, the decoder's frame, the CRC-32 table | 71,488 of 98,304 B |
| Store: static PSRAM | a replay's capture or a dump | 7 MB |

Static DRAM must end below the banks at 0x3fcb0000 (`banks.ld`). Removing Wi-Fi freed the room
for the low arena; the high arena fills the rest. Queues, semaphores, the capture task's stack
and the capture timer are made once at boot from static storage. The timer is made on core 1,
where its interrupt must run.

## How the PHY comes up without Wi-Fi

At boot (`capture_init`):
1. `esp_wifi_bt_power_domain_on()` powers the modem domain.
2. `esp_phy_enable(PHY_MODEM_BT)` turns on the Wi-Fi and Bluetooth clocks itself
   (`phy_module_enable`) and runs the full calibration (`register_chipv7_phy`, ESP-IDF v6.1
   `phy_init.c`). The Bluetooth bit is what the old code held after Wi-Fi stopped. With
   Bluetooth not built, it also asks for no PLL tracking.
3. `wifi_module_enable()` keeps the Wi-Fi clock domain on, because the dump engine is part of the
   Wi-Fi baseband.

Each run still sets bit 1 of `SYSCON_WIFI_BB_CFG_REG`, then tunes and forces the gain as before.
Nothing hung, and the dump ran on the first try. The only remaining Wi-Fi dependency is the
`esp_wifi` component in the build: `wifi_module_enable()` is compiled only with
`CONFIG_ESP_WIFI_ENABLED`, which esp_wifi's Kconfig defines.

## Cost

| | Before (5 Msym/s runs and TS runs) | f77ef5f |
|---|---|---|
| Core 1 per unit, 8PSK 8/9 | 120.8k to 121.3k cycles | 121.5k to 122.1k (+0.6%) |
| Release per frame | 94.3k | 95.2k (+1.0%) |
| Check per frame | 75.2k to 75.6k | 75.5k to 76.4k |

The receiver is a little slower. Where the symbol ring and frame slots now sit may explain it;
that was not measured.

**One failed frame.** While developing the Wi-Fi-free version (the same bring-up, before the
arenas), one frame in 137,488 failed, at 8PSK 8/9 5 Msym/s. It had a single 50 us burst of
confident bit errors (data symbols 257 to 508) while its pilots measured 29.5 dB, and no
neighbour was affected. With Wi-Fi the receiver had shown no such frame in about 430,000. Since then,
277,581 frames without Wi-Fi decoded clean: 103,111 in three more development runs, 34,362 in one
with the arenas, and the 140,108 here. One event cannot tell
outside interference from something the Wi-Fi bring-up used to set.

**membench** lost its GDMA column, the only one that needed ESP-IDF's allocating async memcpy.
