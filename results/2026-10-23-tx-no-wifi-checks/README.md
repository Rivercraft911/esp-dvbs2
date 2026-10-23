# Transmitter without Wi-Fi: level, registers and drift

**Result.** Without Wi-Fi, the transmitter's output level matches the Wi-Fi builds to within
the 0.9 dB spread of the runs. The ID tone measured 54.25 to 55.11 dB without Wi-Fi and 54.56 to
55.13 dB with Wi-Fi, over eight keyed runs. The 1.4 dB drop reported in
[tx-no-wifi](../2026-10-23-tx-no-wifi/README.md) was a measurement artifact. That metric
averaged the Morse ID over a fixed 0.5 s window, and with Wi-Fi the window caught more key-down
time: 136 to 151 of 305 segments, against 110 to 113 without. With Wi-Fi the ID starts later,
because Wi-Fi's start comes first. That cause is inferred from the segment counts. It was not
timed directly.

A register diff after identical keyed setup found 4 stable differences from the Wi-Fi
start/stop path. Restoring the two writable ones changed nothing. One keyed run of 535 s,
with no periodic PHY tracking, decoded every captured frame exactly from start to end. Its
best-frame MER moved by 0.2 dB, carrier leak by 1.2 dB and image by 1 dB.

## Level

`dvb 2420 11 5m 20 0 128` (QPSK 9/10, 5 Msym/s) into the B200 through 40 dB of coax, at
10 MS/s and gain 40. Each run started from a fresh boot. Set `level` in `runs.jsonl`.

The tone level now uses key-down time only. The 0.5 s capture at 2.0 s is split into 16,384-sample
segments. Segments whose tone peak is within 3 dB of the strongest one are averaged. Tone,
image (at minus the tone frequency) and carrier leak are read from that average. Data bursts
are 0.5 s at 8 s, decoded by LeanDVB and regenerated from the recovered xorshift32 state.

| Arm | Firmware | Tone (dB) | Image (dBc) | Carrier (dBc) | Key-down segments | Frames exact | Best MER (dB) |
|---|---|---:|---:|---:|---:|---:|---:|
| No Wi-Fi | 899244a + probe | 54.25 | -55.6 | -29.9 | 110 | 64 / 64 | 19.2 |
| No Wi-Fi, two BB registers set to Wi-Fi values | 899244a + probe | 55.11 | -56.8 | -37.2 | 110 | 64 / 64 | 19.6 |
| No Wi-Fi | 899244a + probe | 54.96 | -58.5 | -33.9 | 110 | 60 / 60 | 19.3 |
| No Wi-Fi, two BB registers set to Wi-Fi values | 899244a + probe | 54.52 | -54.3 | -33.3 | 111 | 62 / 62 | 18.5 |
| Wi-Fi up | b003f26 + probe | 55.13 | -55.3 | -30.4 | 136 | 65 / 65 | 16.8 |
| Wi-Fi started, stopped before keying (`--phy-solo`) | b003f26 + probe | 54.56 | -54.5 | -31.5 | 151 | 61 / 61 | 18.8 |
| Wi-Fi up | b003f26 + probe | 54.93 | -58.5 | -30.6 | 148 | 65 / 65 | 18.0 |

"Probe" is an experiment-only patch, never committed. It adds a console command that reads the
registers and one that writes chosen registers just before the DAC starts. Three further Wi-Fi
runs are in the records with no B200 data: UHD timed out on the first burst. They are excluded
here. Outside 2.4 GHz traffic still reaches the B200, so image, carrier and MER scatter from
run to run in every arm.

The PHY's maximum-power setting without Wi-Fi is 80, in units of 0.25 dBm (`phy_get_most_tpw()`, 20 dBm).
That is CONFIG_ESP_PHY_MAX_WIFI_TX_POWER, Wi-Fi's own default limit. The per-rate power
tables come from the same PHY init data on both paths, through `esp_phy_load_cal_and_init`.

## Registers after the same keyed setup

The registers were read after tuning, the CW calibration tone, `force_tx_gain`, `force_txon_mode`
and the transmit filter, immediately before the DAC would start. The run then aborts, so no
sample plays. The read covers FE2 0x60005000 (512 words), FE 0x60006000 (512), AGC 0x6001c000
(768), NRX 0x6001cc00 (256), BB 0x6001d000 (1,024), SYSCON 0x60026000 (256), DAC 0x60033d00
(64), and analog I2C blocks 0x61 to 0x6d, registers 0 to 31, through both hosts: 4,224 values.
Snapshots: Wi-Fi up 3 (two in one boot, one after a reset), Wi-Fi started and stopped 2, no
Wi-Fi 3. All of them are summarized in `registers.jsonl`.

- 84 registers change between snapshots of the same arm (counters, RSSI). They are ignored.
- No Wi-Fi against Wi-Fi started and stopped: 4 stable differences.
  - 0x6001c138 (AGC) reads 0xdca004ca against 0xdc9ff4ca. Writes do not stick, so this is a status register.
  - 0x6001d048 and 0x6001d04c (BB) read 6 and 0 against 7 and 1. Writing 7 and 1 before keying
    did not change the tone (table above).
  - 0x60033d14 reads 0 against 1, a status bit (also seen on the receiver).
- No Wi-Fi against Wi-Fi up: those 4, plus 12 more that a running Wi-Fi sets. These are FE
  words 0x6000610c and 0x60006128, each in four mirrored copies, and 0x6001c08c, 0x6001d008,
  0x6002600c and 0x60033d84.
- The 16-byte FE table at 0x60006024 to 0x60006030 that differed on the receiver
  ([rx-no-wifi-ab](../2026-10-23-rx-no-wifi-ab/README.md)) is equal in every arm here. The
  transmitter's one `phy_param_track_tot(true, false)` call before keying restores it.

## 535 s without periodic tracking

`dvb 2420 11 5m 540 0 128` on 60e722f: two 4.98 s IDs and 525.06 s of data, 535.02 s in all.
Set `drift`. Wi-Fi used to repeat the tracking pass every second. This firmware makes it once,
before keying.

| Capture at | Frames exact | Best / median MER (dB) |
|---|---:|---|
| 8 s | 57 / 57 | 17.9 / 17.6 |
| 20 s | 55 / 55 | 18.1 / 16.35 |
| 270 s | 65 / 65 | 17.2 / 16.4 |
| 505 s | 62 / 62 | 17.7 / 17.25 |
| 515 s | 62 / 62 | 18.2 / 15.2 |
| 525 s | 54 / 54 | 17.7 / 16.6 |

| ID | Tone (dB) | Image (dBc) | Carrier (dBc) | Carrier offset (kHz) |
|---|---:|---:|---:|---:|
| Opening (2.0 s) | 54.40 | -59.0 | -30.4 | -29.9 |
| Closing (531.5 s) | 53.55 | -60.0 | -29.2 | -33.0 |

The 3 kHz carrier shift over nine minutes is a reference drift, in the transmitter or the B200.
The level change of -0.85 dB is close to the run-to-run spread above. Median MER follows
the outside traffic. Best-frame MER shows no trend.
