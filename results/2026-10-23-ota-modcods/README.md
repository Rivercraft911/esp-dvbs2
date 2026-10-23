# Normal-frame MODCOD sweep at 5 Msym/s (partial)

**Result.** Six of the fourteen planned MODCODs ran before the bench was taken down: QPSK 1/2,
3/5, 2/3, 3/4, 4/5 and 5/6 (MODCOD 4 to 9). Each was keyed for 30 s with a fresh xorshift
payload and captured whole, and LeanDVB decoded 18,021 frames, every one exact and contiguous.
QPSK 8/9 and 9/10 and all six 8PSK MODCODs (10 to 17) did not run: the B200 was disconnected
for another setup partway through the sweep, and the capture for MODCOD 10 failed. That run has
no record. It may have keyed the board while the B200 was being removed; no capture shows it.
8PSK 2/3 and 9/10 at 5 Msym/s have keyed records in [tx-table](../2026-10-23-tx-table/README.md).

The transmitter accepts MODCOD 4 to 17 only; `dvbdump 1 5m 1`, `2`, `3` and `18` are refused
with the usage line, so QPSK 1/4, 1/3 and 2/5 are not part of the sweep.

## Setup

- XIAO ESP32-S3 44:B1:76:B0:E5:8C, firmware 517c641, callsign KJ5SCH. `dvb 2420 MODCOD 5m 30
  0 128`: two 4.98 s IDs and 20.04 s of data. Coax with 40 dB of marked attenuation to the B200
  (34A6B8E), TX/RX port, receive only, gain 30 dB.
- Continuous sc16 capture at 10 MS/s from the keyed command to 3 s after the run. No overflow
  and no lost sample in any of the six.
- LeanDVB (the build with the corrected short 3/4 table) over the whole capture, with
  `host/decode.py`'s options.
- Check: every frame carries the station header, then the xorshift32 stream regenerated from
  the state solved from six payload bytes; each frame must continue the previous one. Stepping
  the stream from its seed gives each frame's number in the run.

## Runs

| MODCOD | Payload rate | Played | Decoded | Frames decoded | Exact, contiguous | MER (dB) | Carrier offset | Encoder spare | Fill spare |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|
| 4, QPSK 1/2 | 4.83 Mb/s | 3,011 | 3,003 | 7 to 3,009 | 3,003 | 20.2 | -27.6 kHz | 91.8% | 55.9% |
| 5, QPSK 3/5 | 5.80 Mb/s | 3,011 | 3,003 | 7 to 3,009 | 3,003 | 20.1 | -29.1 kHz | 91.1% | 55.9% |
| 6, QPSK 2/3 | 6.45 Mb/s | 3,011 | 3,007 | 3 to 3,009 | 3,007 | 20.2 | -30.0 kHz | 91.3% | 55.9% |
| 7, QPSK 3/4 | 7.26 Mb/s | 3,011 | 3,003 | 7 to 3,009 | 3,003 | 20.2 | -30.6 kHz | 90.8% | 55.9% |
| 8, QPSK 4/5 | 7.75 Mb/s | 3,011 | 3,002 | 8 to 3,009 | 3,002 | 20.2 | -30.7 kHz | 90.6% | 55.9% |
| 9, QPSK 5/6 | 8.08 Mb/s | 3,011 | 3,003 | 7 to 3,009 | 3,003 | 19.9 | -30.8 kHz | 90.7% | 55.9% |

- Played is the board's `frames_played`. 20.04 s holds 3,010.6 QPSK frames of 6.6564 ms, so
  frames 0 to 3,009 are whole and frame 3,010 is cut by the closing ID. Every run decoded up to
  the last whole frame; LeanDVB found its first frame 3 to 8 frames after the opening ID.
- 0 BCH failures and 0 LDPC corrections between the first and last decoded frame in every run.
  MER is LeanDVB's PLHEADER MER, the median over that stretch. Carrier offset is LeanDVB's
  median frequency estimate, the S3 against the B200's own reference.
- Spare is 1 minus the mean encoder cycles over the frame period, and 1 minus the slowest bank
  fill over the bank deadline, at 240 MHz. No late fill and no dummy frame in any run.

## Records

`runs.jsonl`: one record per run (board completion record, B200 settings and capture status,
LeanDVB report, check).
