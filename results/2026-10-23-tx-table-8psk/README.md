# Keyed table, 8PSK at 26.67 Msym/s after the 8PSK encoder work

An addendum to [tx-table](../2026-10-23-tx-table/README.md). The method, setup and checks are
the same: 30 s keyed into the B200's TX/RX port at gain 30, four captures, LeanDVB, every frame
regenerated or matched against its source. Firmware 05510f7 merges the 8PSK encoder work
(exp/tx-8psk 796ec1c) into exp/tx-clean.

**Result.** With a fresh payload, all three 8PSK rates at 26.67 Msym/s now keep at least 15% on
both cores. Their encoder reserve is 20.2%, 16.5% and 21.4%, their bank-fill reserve 19.8 to
20.5%, and they sent no dummy frames. All 2,840 decoded frames were exact. With TS input
replayed, 8PSK 9/10 runs at full rate with 2.7% encoder spare and 4 dummy frames, delivering
69.8 Mb/s, up from 64.8. QPSK 9/10 at 33.33 Msym/s with TS input replayed keeps 14.4% encoder
spare and 14.9% fill spare.

| Mode | Payload rate | Exact / decoded | Longest run | MER best / median (dB) | Fill reserve | Encoder reserve | Dummies |
|---|---:|---:|---:|---|---:|---:|---:|
| 8PSK 2/3, 26.67 Msym/s | 51.62 Mb/s | 946 / 946 | 238 | 20.3 / 17.1 | 20.3% | 20.2% | 0 |
| 8PSK 5/6, 26.67 Msym/s | 64.59 Mb/s | 946 / 946 | 237 | 18.9 / 17.1 | 19.8% | 16.5% | 0 |
| 8PSK 9/10, 26.67 Msym/s | 69.82 Mb/s | 948 / 948 | 237 | 18.4 / 18.0 | 20.5% | 21.4% | 0 |
| 8PSK 9/10, 26.67 Msym/s, TS input replayed | 69.82 Mb/s | 947 / 947 | 237 | 17.9 / 17.6 | 18.5% | 2.7% | 4 |
| QPSK 9/10, 33.33 Msym/s, roll-off 0.20, TS input replayed | 58.20 Mb/s | 794 / 794 | 199 | 16.3 / 15.1 | 14.9% | 14.4% | 0 |

Against the same rows in tx-table (95f2469): the 8PSK encoder means fell from 186.0k, 195.0k
and 177.6k cycles a frame to 159.4k, 166.8k and 157.0k. The frame lasts 199,746 cycles. The
slowest bank fill fell from 39.5 to 41.4k cycles to 37.5 to 37.9k, against a 47,220-cycle
deadline. Every one of the 20 captures here succeeded. Each run's payload rate counts the
frames it played in 20.04 s of data.
