# Normal-frame MODCOD sweep at 5 Msym/s, the rest

**Result.** The eight MODCODs the [first part](../2026-10-23-ota-modcods/README.md) missed all
decoded from the air with every frame exact and contiguous: QPSK 8/9 and 9/10 and 8PSK 3/5 to
9/10 (MODCOD 10 to 17), 30 s keyed each with a fresh xorshift payload, 32,997 frames, 0 wrong,
0 gaps. With the first part, every normal-frame MODCOD the transmitter accepts (4 to 17) now
has a keyed, fully captured and fully decoded run at 5 Msym/s.

This part used a different transmitter board. The first part keyed XIAO 44:B1:76:B0:E5:8C on
firmware 517c641 through 40 dB. When the bench was rebuilt, the board cabled to the B200 was
44:B1:76:B0:DA:C8, until then the S3 receiver, through 30 dB. I flashed it with the
transmitter from main (a457805, XIAO overlay); its sources are the same as 517c641's.

## Setup

- Transmitter: XIAO ESP32-S3 44:B1:76:B0:DA:C8. Before flashing it ran the receiver app
  (`esp_dvbs2_rx`, firmware 517c641; `info` saved in `runs.jsonl`). Flashed with the
  transmitter a457805 built with `sdkconfig.defaults;sdkconfig.xiao` after `idf.py
  reconfigure`. I stored KJ5SCH and reset it; `info` then read callsign KJ5SCH, firmware
  a457805, run memory ready and 0 heap calls after boot. The board is left on this firmware.
- `dvb 2420 MODCOD 5m 30 0 128`: two 4.98 s IDs and 20.04 s of data.
- Path: coax with 30 dB of marked attenuation to the B200 (34A6B8E), TX/RX port, receive
  only, B200 gain 20 dB. Continuous sc16 capture at 10 MS/s from the keyed command to 3 s after
  the run, under `caffeinate -i -s -d`. No overflow and no lost sample in any run.
- LeanDVB (the build with the corrected short 3/4 table) over the whole capture, with
  `host/decode.py`'s options. The check is the first part's: station header, xorshift32 stream
  regenerated from the solved state, each frame continuing the previous one, frame numbers
  from the seed.

## Level check

Before the sweep, one 15 s keyed run of 8PSK 2/3 checked the new path for clipping. The largest
I or Q sample was 7,459 of 32,767 (12.9 dB below full scale) during the opening ID and 4,692
during data; the data's RMS was -21.2 dBFS. The ID tone peaked at -13.6 dBFS, 6.2 dB below
e5:8c's through the old 40 dB path at gain 30, so this board and path deliver a little less.
LeanDVB decoded 1,134 frames, all exact (frames 1 to 1,134 of 1,135 whole ones), and the
opening ID decoded as KJ5SCH. The [path check](../2026-10-23-ota-path-check/README.md) explains
why e5:8c was not used.

## Runs

| MODCOD | Payload rate | Played | Decoded | Frames decoded | Exact, contiguous | MER (dB) | Carrier offset | Encoder spare | Fill spare |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|
| 10, QPSK 8/9 | 8.62 Mb/s | 3,011 | 3,005 | 5 to 3,009 | 3,005 | 20.8 | -23.7 kHz | 89.6% | 55.9% |
| 11, QPSK 9/10 | 8.73 Mb/s | 3,011 | 3,004 | 6 to 3,009 | 3,004 | 21.2 | -27.0 kHz | 90.4% | 55.9% |
| 12, 8PSK 3/5 | 8.70 Mb/s | 4,515 | 4,477 | 37 to 4,513 | 4,477 | 20.4 | -28.6 kHz | 80.0% | 55.9% |
| 13, 8PSK 2/3 | 9.68 Mb/s | 4,515 | 4,504 | 10 to 4,513 | 4,504 | 20.7 | -29.4 kHz | 76.8% | 55.9% |
| 14, 8PSK 3/4 | 10.89 Mb/s | 4,515 | 4,498 | 16 to 4,513 | 4,498 | 20.8 | -29.8 kHz | 78.5% | 55.9% |
| 15, 8PSK 5/6 | 12.11 Mb/s | 4,515 | 4,496 | 18 to 4,513 | 4,496 | 20.7 | -29.9 kHz | 76.2% | 55.9% |
| 16, 8PSK 8/9 | 12.93 Mb/s | 4,515 | 4,514 | 0 to 4,513 | 4,514 | 20.7 | -29.9 kHz | 79.3% | 55.9% |
| 17, 8PSK 9/10 | 13.09 Mb/s | 4,515 | 4,499 | 15 to 4,513 | 4,499 | 20.9 | -30.0 kHz | 74.1% | 55.9% |

- Played is the board's `frames_played`; its last frame is cut by the closing ID. 20.04 s holds
  3,010.6 QPSK frames (6.6564 ms) or 4,514.7 8PSK frames (4.4388 ms), so the last whole frame
  is 3,009 or 4,513, and every run decoded up to it. The first frames lost are LeanDVB finding
  its lock after the opening ID: 0 to 18 frames, and 37 for 8PSK 3/5.
- 0 BCH failures and at most 19 LDPC bit corrections among each run's decoded frames. For 8PSK
  3/5, `payload_fec` in the record counts 541 failures, but they all come after the data, in
  the closing ID's noise, ahead of one MODCOD 1 attempt that passed BCH on noise and produced no
  BBFRAME. The 4,477 MODCOD 12 attempts after the first good one all passed.
- MER is LeanDVB's PLHEADER MER, the median between the first and last frame that passed BCH
  (for 8PSK 3/5 that stretch runs on to the false pass in the closing ID). Carrier
  offset is its median frequency estimate, da:c8 against the B200's own reference. It moved from
  -23.7 to -30.0 kHz over the eight back-to-back runs.
- Spare is 1 minus the mean encoder cycles over the frame period, and 1 minus the slowest bank
  fill over the bank deadline, at 240 MHz. No late fill and no dummy frame in any run.
- The in-band level was -89.0 to -90.2 dBFS/Hz at B200 gain 20.

## Records

`runs.jsonl`: da:c8's `info` before and after flashing, the level check, then one record per
sweep run (board completion record, B200 settings and capture status, LeanDVB report, check).
