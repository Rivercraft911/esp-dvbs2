# Core-1 fill work, keyed

**Result.** Keyed into the B200, every 80 MS/s mode in this record kept at least 16.2% of its
bank deadline free on core 1. Each decoded every frame exactly, with no late fill, late switch
or dummy frame. Altogether 8,961 frames were decoded, all exact.

This confirms on air the radio-off result of [tx-fill](../2026-10-23-tx-fill/README.md), at
least 17.2% in every mode. The keyed spare is within 1 point of the paces in each mode.

A 540 s keyed run of QPSK 9/10 at 33.33 Msym/s also had no late fill over its full length. Its
longest fill took 39,104 of 47,220 cycles (17.2% spare), and five captures from 8 s to 525 s
were exact. The command finished 0.76 s later, relative to its playback, than the 30 s runs
did. That fits the core-1 tick catch-up that tx-fill estimated at about 0.8 s. The catch-up
disturbed nothing: the transmitter was already off, and the next run was normal.

Firmware ed0db9f (main) ran on the XIAO ESP32-S3 44:B1:76:B0:E5:8C. It includes the fill
commits 9880092 and b96247a, and bdf9cf6, the TS roll-off fix.

## Setup

The method and checks are those of [tx-table](../2026-10-23-tx-table/README.md):

- `dvb 2420 MODE 30 0 48` with a fresh payload: the on-chip xorshift32 stream, or TS input
  replayed through the real parser after `spiinput quad ts replay`.
- Coax with 40 dB marked attenuation into the B200's TX/RX port, receive only, gain 30.
  Callsign KJ5SCH.
- Four 0.2 s captures at 7, 12, 17 and 22 s, decoded by LeanDVB. Every xorshift32 frame is
  regenerated from its recovered state. Every TS user packet is matched against the replay
  pattern, in order.
- With the transmitter idle before each row, no 100 us block rose 10 dB above the quietest
  tenth.
- Fill reserve is 1 - longest bank fill / 47,220 cycles. Min slack is `min_fill_slack_cycles`.
  Encoder reserve is 1 - mean encoder cycles / frame period. All three come from each run's
  own completion record.

## Table

| Mode | Payload rate | Exact / decoded | Longest run | MER best / median (dB) | Fill reserve | Min slack | Encoder reserve | Dummies | Late fills |
|---|---:|---:|---:|---|---:|---:|---:|---:|---:|
| QPSK 9/10, 33.33 Msym/s, roll-off 0.20 | 58.20 Mb/s | 793 / 793 | 199 | 18.0 / 16.8 | 17.7% | 8,068 | 30.1% | 0 | 0 |
| the same, keyed again after the 540 s run | 58.20 Mb/s | 793 / 793 | 199 | 17.1 / 15.9 | 17.6% | 8,022 | 30.1% | 0 | 0 |
| the same, TS input replayed | 58.20 Mb/s | 793 / 793 | 199 | 17.9 / 16.9 | 16.2% | 7,358 | 15.5% | 0 | 0 |
| the same, 540 s | 58.20 Mb/s | 992 / 992 | 199 | 17.3 / 16.3 | 17.2% | 7,822 | 30.1% | 0 | 0 |
| QPSK 9/10, 26.67 Msym/s | 46.56 Mb/s | 632 / 632 | 158 | 22.1 / 18.8 | 20.7% | 9,465 | 59.3% | 0 | 0 |
| 8PSK 2/3, 26.67 Msym/s | 51.62 Mb/s | 946 / 946 | 237 | 20.7 / 19.3 | 21.9% | 9,756 | 20.5% | 0 | 0 |
| 8PSK 5/6, 26.67 Msym/s | 64.59 Mb/s | 947 / 947 | 237 | 20.2 / 19.1 | 21.8% | 8,441 | 16.8% | 0 | 0 |
| 8PSK 9/10, 26.67 Msym/s | 69.82 Mb/s | 947 / 947 | 237 | 20.0 / 19.0 | 22.7% | 10,158 | 21.5% | 0 | 0 |
| 8PSK 2/3, 20 Msym/s | 38.71 Mb/s | 706 / 706 | 177 | 19.5 / 18.9 | 17.3% | 7,871 | 49.2% | 0 | 0 |
| 8PSK 5/6, 20 Msym/s | 48.45 Mb/s | 706 / 706 | 177 | 20.0 / 18.2 | 17.4% | 7,923 | 40.2% | 0 | 0 |
| 8PSK 9/10, 20 Msym/s | 52.37 Mb/s | 706 / 706 | 178 | 18.4 / 18.1 | 17.5% | 7,980 | 53.8% | 0 | 0 |

No run had a late switch or an underrun, and every run returned `ESP_OK`. All 45 captures
decoded, and each was exact from its first frame to its last.

**Against earlier records.** The keyed fill spare before the fill work, and the radio-off paces
after it:

| Mode | Keyed before | Radio off, b96247a ([tx-fill](../2026-10-23-tx-fill/README.md)) | Keyed now |
|---|---:|---:|---:|
| QPSK 9/10, 33.33 Msym/s | 14.2% (b778765) | 17.4% | 17.2% to 17.7% |
| the same, TS input replayed | 15.4% (3679c73) | not paced | 16.2% |
| QPSK 9/10, 26.67 Msym/s | 18.6% (b778765) | 20.6% | 20.7% |
| 8PSK 2/3, 5/6, 9/10, 26.67 Msym/s | 20.4%, 19.8%, 20.2% (cc2d14b) | 22.9%, 22.3%, 22.8% | 21.9%, 21.8%, 22.7% |
| 8PSK 2/3, 5/6, 9/10, 20 Msym/s | 15.5%, 15.5% (b778765), 15.7% (cc2d14b) | 17.5%, 17.4%, 17.6% | 17.3%, 17.4%, 17.5% |

The "Keyed before" values come from [tx-table](../2026-10-23-tx-table/README.md),
[tx-table-8psk-rekey](../2026-10-23-tx-table-8psk-rekey/README.md) and
[tx-ts-input](../2026-10-23-tx-ts-input/README.md).

## 540 s and the tick catch-up

`dvb 2420 11 33m333 540 0 48` played 535.020 s of samples: two 4.98 s IDs and 525.06 s of
data. It encoded 525,871 frames and played 525,869. Captures at 8, 135, 270, 405 and 525 s
decoded 198, 198, 199, 198 and 199 frames, all exact. The encoder's mean was 167,431 cycles
and its slowest frame took 189,428 of 239,630.

During a run, core 1's FreeRTOS tick is off. When it comes back on after the RF cleanup,
IDF's handler runs every skipped tick at once. That happens on core 1, before the worker reports
the run done. Here it was measured from the host, as the time from sending the command to its
`done` record (`command_seconds` in `runs.jsonl`), minus the playback time:

| Run | Playback (s) | Command (s) | Overhead (s) |
|---|---:|---:|---:|
| Nine 30 s rows and the repeat | 30.000 | 30.130 to 30.226 | 0.130 to 0.226, mean 0.183 |
| 540 s | 535.020 | 535.959 | 0.939 |

The 540 s run's overhead exceeds the 30 s mean by 0.76 s. The spread of the 30 s runs puts that
between 0.71 and 0.81 s.

tx-fill's two radio-off points give a model of the catch-up: 29 ms at 10 s, 100 ms at 60 s, and
1.43 ms per further second. That model predicts 0.78 s at 535 s and 0.06 s at 30 s, a
difference of 0.72 s. The measured 0.76 s agrees within the host timing spread, so the
catch-up after a 540 s run is about 0.8 s.

It disturbed nothing:

- The run reported `ESP_OK` with no late fills.
- The transmitter is off by then. The RF cleanup comes before the tick is re-enabled.
- The 30 s QPSK run keyed right after it had 17.6% fill spare and decoded 793 of 793 frames.

## TS roll-off in the BBHEADER

The TS replay row is the first keyed TS run with correct BBHEADERs. All 793 decoded frames
carry roll-off code 2 (0.20) in MATYPE-1, matching the 0.20 pulse.

The keyed TS run in [tx-ts-input](../2026-10-23-tx-ts-input/README.md) (3679c73) predates
bdf9cf6. Its 793 frames carried code 0 (0.35) while shaped at 0.20, read from the same
LeanDVB BBFRAME output. LeanDVB decoded them anyway, so it ignores the roll-off field. That
record is left as it was run.

## Records

`runs.jsonl` holds one `row` summary per row, followed by its full `run` record:

- the board's completion record
- every capture's B200 status
- the LeanDVB report and the check
- `command_seconds`

The row name `qpsk910-33m333` appears twice. The second instance is the run keyed after the
540 s run.
