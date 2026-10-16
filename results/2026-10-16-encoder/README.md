# First C encoder on the ESP32-S3

Firmware `fd3af79` (branch exp/encoder), ESP-IDF v6.1, 240 MHz, encoder on core 1, hot stages in
IRAM. `python3 host/board.py fecbench`, records in `runs.jsonl`, checked by
`python3 tests/test_fecbench.py results/2026-10-16-encoder/runs.jsonl`.

**Correct:** all 140 output CRCs (14 modes x GCS and TS input x 5 frames) equal the Python
reference. Normal frames with pilots, PL scrambling code 0.

| Mode, GCS input | Median cycles per frame | Share of one core |
|---|---:|---:|
| QPSK 1/2 | 2.27 M | 28% at 1 Msym/s |
| QPSK 2/3 | 2.18 M | 27% at 1 Msym/s |
| QPSK 9/10 | 2.03 M | 25% at 1 Msym/s |
| 8PSK 2/3 | 2.35 M | 221% at 5 Msym/s |
| 8PSK 5/6 | 2.32 M | 218% at 5 Msym/s |

One core has 240e6 x 33282 / 1e6 = 7.99 M cycles per QPSK frame at 1 Msym/s and
240e6 x 22194 / 5e6 = 1.07 M per 8PSK frame at 5 Msym/s. TS input adds 0.1–0.18 M cycles per
frame for the packet CRC-8.

So the first milestone, QPSK at 1 Msym/s, already fits with three quarters of a core to spare.
8PSK video needs this encoder about 2.3 times faster, or split across both cores. Per 8PSK 2/3
frame the time goes to interleaving and mapping (0.91 M), PL framing (0.52 M), BCH (0.36 M) and
LDPC (0.51 M), all written as clear bit-level loops. For comparison, the optimized RP2350 version
ran BCH and LDPC in about 0.15 M cycles per frame at 128 MHz.
