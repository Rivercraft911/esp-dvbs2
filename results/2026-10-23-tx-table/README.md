# Keyed table

**Result.** Every transmitter mode decoded from the air with every frame exact: 19 keyed
runs, 10,219 frames recovered by LeanDVB, 0 wrong. Every run carried a fresh payload, never a
repeated frame. That payload was either the on-chip xorshift32 stream or payload input
replayed through the real SPI parser. Payload rates run from 0.97 Mb/s (QPSK 1/2, 1 Msym/s) to
69.8 Mb/s (8PSK 9/10, 26.67 Msym/s).

Two things fall short. Two 80 MS/s modes keep less than 15% bank-fill spare: QPSK 9/10 at
33.33 Msym/s has 12.4 to 14.2%, and 8PSK 5/6 at 26.67 Msym/s has 12.4%. With a fresh payload,
8PSK at 26.67 Msym/s keeps 2.4 to 11.1% encoder spare, below 15%, and 5/6 sent 6 dummy frames
in 20 s. With TS input replayed, 8PSK 9/10 at 26.67 Msym/s does not keep up: it sent 11,516
dummy frames and delivered 64.8 of its 69.8 Mb/s. QPSK 9/10 at 33.33 Msym/s keeps 26.1% encoder
spare with a fresh payload, 23.5% with GCS input replayed and 13.0% with TS input replayed.

The receiver port mattered more than anything else on the bench (next section).

## Setup

- ESP32-S3 (XIAO) → coax with 40 dB marked attenuation → USRP B200, **TX/RX port, receive
  only**, gain 30 dB. 2420 MHz, transmit gain 0, peak 128 at 40 MS/s and 48 at 80 MS/s,
  callsign KJ5SCH.
- Each row: `dvb 2420 MODE 30 ...`, which is two 4.98 s IDs and 20.04 s of data. Before keying,
  0.5 s of the band was recorded with the transmitter idle. During the run, four bursts were
  captured at 7, 12, 17 and 22 s (1.0 s at 1 Msym/s, 0.5 s at 5 Msym/s, 0.3 s at 10 Msym/s,
  0.2 s above that). B200 rate: 4 MS/s for 1 Msym/s, then 10, 20 and 50 MS/s. LeanDVB used
  2x resampling below two samples per symbol.
- Checks: xorshift32 frames are regenerated from a state recovered from 4 payload bytes and
  must continue the stream. Every user packet of a TS replay frame must equal a replay-pattern
  packet and follow the pattern order. Every GCS replay frame must be the next 7,264 bytes of
  the replay stream. "Longest run" counts consecutive exact frames that continue each other.
- MER is what LeanDVB reports: the best value in a run and the median over its reports.
- Timing columns come from each run's own record. Fill reserve is 1 - (slowest bank fill / bank
  deadline). Encoder reserve is 1 - (mean encoder cycles / frame period). Dummies are the
  steady dummy frames sent while the encoder was late.

## Table

| Mode | Symbol rate | Bandwidth | Payload rate | Exact / decoded | Longest run | MER best / median (dB) | Fill reserve | Encoder reserve | Dummies | Firmware |
|---|---:|---:|---:|---:|---:|---|---:|---:|---:|---|
| QPSK 1/2 | 1 Msym/s | 1.35 MHz | 0.97 Mb/s | 111 / 111 | 28 | 24.1 / 21.6 | 51.4% | 91.6% | 0 | b778765 |
| 8PSK 2/3 | 5 Msym/s | 6.75 MHz | 9.68 Mb/s | 334 / 334 | 108 | 21.0 / 19.3 | 55.4% | 76.1% | 0 | b778765 |
| 8PSK 9/10 | 5 Msym/s | 6.75 MHz | 13.09 Mb/s | 337 / 337 | 110 | 20.3 / 19.2 | 55.3% | 73.7% | 0 | b778765 |
| 8PSK 8/9, short frames | 5 Msym/s | 6.75 MHz | 12.64 Mb/s | 1,697 / 1,697 | 433 | 21.0 / 19.4 | 55.5% | 76.4% | 0 | b778765 |
| QPSK 9/10 | 10 Msym/s | 13.5 MHz | 17.46 Mb/s | 353 / 353 | 89 | 20.4 / 19.4 | 49.7% | 80.1% | 0 | b778765 |
| QPSK 3/4, TS input replayed | 10 Msym/s | 13.5 MHz | 14.52 Mb/s | 263 / 263 | 88 | 21.1 / 19.3 | 49.8% | 74.3% | 0 | 95f2469 |
| 8PSK 5/6 | 10 Msym/s | 13.5 MHz | 24.22 Mb/s | 406 / 406 | 133 | 19.8 / 18.8 | 50.1% | 69.5% | 0 | b778765 |
| QPSK 9/10 | 20 Msym/s | 27 MHz | 34.92 Mb/s | 227 / 227 | 118 | 20.9 / 19.8 | 23.5% | 68.5% | 0 | b778765 |
| 8PSK 2/3 | 20 Msym/s | 27 MHz | 38.71 Mb/s | 506 / 506 | 176 | 19.5 / 17.6 | 15.5% | 47.5% | 0 | b778765 |
| 8PSK 5/6 | 20 Msym/s | 27 MHz | 48.45 Mb/s | 531 / 531 | 177 | 17.7 / 16.6 | 15.5% | 38.9% | 0 | b778765 |
| 8PSK 9/10 | 20 Msym/s | 27 MHz | 52.37 Mb/s | 360 / 360 | 177 | 17.3 / 17.0 | 15.1% | 3.9% | 0 | b778765 |
| QPSK 9/10 | 26.67 Msym/s | 36 MHz | 46.56 Mb/s | 476 / 476 | 159 | 20.8 / 19.6 | 18.6% | 58.1% | 0 | b778765 |
| 8PSK 2/3 | 26.67 Msym/s | 36 MHz | 51.62 Mb/s | 711 / 711 | 238 | 19.6 / 17.1 | 16.1% | 6.9% | 0 | 95f2469 |
| 8PSK 5/6 | 26.67 Msym/s | 36 MHz | 64.59 Mb/s | 948 / 948 | 237 | 19.4 / 17.5 | 12.4% | 2.4% | 6 | 95f2469 |
| 8PSK 9/10 | 26.67 Msym/s | 36 MHz | 69.82 Mb/s | 702 / 702 | 237 | 18.5 / 17.2 | 16.4% | 11.1% | 0 | 95f2469 |
| 8PSK 9/10, TS input replayed | 26.67 Msym/s | 36 MHz | 64.81 Mb/s delivered | 669 / 669 | 220 | 18.9 / 17.7 | 16.7% | -5.8% | 11,516 | 95f2469 |
| QPSK 9/10, roll-off 0.20 | 33.33 Msym/s | 40 MHz | 58.20 Mb/s | 587 / 587 | 199 | 16.9 / 16.1 | 14.2% | 26.1% | 0 | b778765 |
| QPSK 9/10, roll-off 0.20, GCS input replayed | 33.33 Msym/s | 40 MHz | 58.20 Mb/s | 595 / 595 | 199 | 17.2 / 16.7 | 12.4% | 23.5% | 0 | 95f2469 |
| QPSK 9/10, roll-off 0.20, TS input replayed | 33.33 Msym/s | 40 MHz | 58.20 Mb/s | 406 / 406 | 198 | 17.1 / 15.2 | 13.6% | 13.0% | 0 | 95f2469 |

Roll-off is 0.35 unless noted. Payload rate is (Kbch - 80) bits per frame at the symbol rate,
with pilots, without ID time. Every row delivered exactly that, counted from the frames each run
played in its 20.04 s of data. The exception is the 8PSK TS replay, whose dummies cost 5 Mb/s.
Firmware b778765 and 95f2469 encode synthetic payloads identically. 95f2469 adds cached BCH
with payload input (b2d0785) and the TS CRC-8 kernel. Their TS replay dumps have the same CRCs.

The longest runs equal whole captures, apart from captures that failed. 22 of the 76 data
captures overflowed the B200 or timed out (`capture` in each burst record). They decoded
nothing or a partial burst and contributed no wrong frame. Not counting those, every capture
decoded throughout.

8PSK 2/3 and 5/6 at 20 Msym/s keep 15.5% fill spare. That clears the 15% bar for keeping them.

## The receiver port, and the interference seen earlier today

The coax connects to the B200's TX/RX port. Captures earlier today used RX2, where the signal
arrives only by leakage. That includes [tx-no-wifi](../2026-10-23-tx-no-wifi/README.md),
[tx-no-wifi-checks](../2026-10-23-tx-no-wifi-checks/README.md) and the first attempt at this
table. On RX2 the signal was about 19 dB weaker: the ID tone was 59.9 dB on RX2 and 78.7 dB on
TX/RX at gain 40, the same run on 85dc467. Outside 2.4 GHz traffic then looked as strong as the
signal. 8PSK at 20 Msym/s and above did not decode, and QPSK at 33.33 Msym/s broke into runs of
12 to 17 frames.

On TX/RX at gain 30 the band is clean. With the transmitter idle, 0.0 to 0.4% of 100 us blocks
rise 10 dB above the quietest tenth, and the in-band level is -131 dBFS/Hz, the receiver's
floor. The comparisons in those two records still hold, since every arm there used RX2.
Their absolute MER, image and carrier figures came through the leakage path. The
earlier note that the S3-to-S3 link at 2420 MHz leaked into the B200 (bursts to -30 dB on a
-51 dB floor) was also taken on RX2. It may be the same leakage, not coupling into the coax
path. It was not rechecked on TX/RX.

**Path level against Oct 20.** Rebuilt today, the Oct 20 firmware (85dc467,
`dvb 2420 11 33m333 20 0 48 --fixture`) gives -79.2 dBFS/Hz in-band on TX/RX at gain 40.
[rf-figures](../2026-10-21-rf-figures/README.md) recorded -89.1 dBFS/Hz for the same firmware,
mode, port and gain. The B200 now receives about 10 dB more than on Oct 20. That fits about
30 dB of path loss now, if the Oct 20 path was the marked 40 dB, but the absolute loss was not
measured. At gain 40 the CW ID tone compresses (+3.8 dB for +10 dB of gain), so these runs
used gain 30. Set `level` in `runs.jsonl` holds these checks. One open point: with `--fixture`,
the rebuilt Oct 20 image did not decode at any gain on the clean TX/RX path, while the current
firmware decodes the same mode. That was not investigated.

## Records

`runs.jsonl`: one `row` summary per table row (idle band, timing, per-burst counts) followed by
its full `run` record. A `run` record holds the board's completion record, every capture's
B200 status, the LeanDVB report and the check. Set `level` holds the port and level checks.
