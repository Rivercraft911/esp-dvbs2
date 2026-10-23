# Self-check before the over-the-air runs

**Result.** All 30 checks of [examples/self-check](../../examples/self-check/README.md) passed on
firmware 517c641, the transmitter used for every over-the-air record of 2026-10-23 that starts
with `ota-`. That is 7 golden dumps, each decoded exactly by LeanDVB, 12 paces with no late bank
and no dummy frame, the VCM dump and its decode, and no heap call after boot. Nothing was keyed.

## Setup

- Board: XIAO ESP32-S3, MAC 44:B1:76:B0:E5:8C, built from main 517c641 with the XIAO overlay
  (`sdkconfig.defaults;sdkconfig.xiao`) after `idf.py reconfigure`. `info` reports `517c641`.
  The firmware sources are the same as a85c2f6, which the board ran before.
- The self-check needs N0CALL stored. I stored it for this run, then stored KJ5SCH again before
  any keyed run; `info` read back KJ5SCH.
- Receiver: the pinned stock LeanDVB build (`examples/self-check/build_receiver.py`).

## Paces

Spare is 1 minus the mean encoder cycles over the frame period, and 1 minus the slowest bank
fill over the bank deadline, at 240 MHz.

| Pace | Encoder spare | Fill spare |
|---|---:|---:|
| QPSK 1/2, 1 Msym/s, 5 s | 91.6% | 52.6% |
| QPSK 9/10, 5 Msym/s, 5 s | 90.3% | 55.8% |
| QPSK 9/10, 10 Msym/s, 5 s | 80.7% | 51.2% |
| QPSK 9/10, 20 Msym/s, 5 s | 69.2% | 25.9% |
| QPSK 9/10, 26.67 Msym/s, 5 s | 59.0% | 21.0% |
| QPSK 9/10, 33.33 Msym/s, 10 s | 30.0% | 17.8% |
| 8PSK 9/10, 5 Msym/s, 5 s | 76.3% | 55.8% |
| 8PSK 2/3, 10 Msym/s, 5 s | 74.5% | 51.4% |
| 8PSK 5/6, 20 Msym/s, 5 s | 39.8% | 17.4% |
| 8PSK 2/3, 26.67 Msym/s, 10 s | 20.3% | 22.8% |
| 8PSK 5/6, 26.67 Msym/s, 10 s | 16.6% | 22.7% |
| 8PSK 9/10, 26.67 Msym/s, 10 s | 21.4% | 22.1% |
| VCM 4/11/13/17, 5 Msym/s, 5 s | 68.6% | 23.0% |

`runs.jsonl` holds every command record and `summary.tsv` the verdicts. Dump IQ and receiver
logs stay out of the repository.
