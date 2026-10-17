# GDMA as a sample engine: correct, but too slow

Every DMA-built bank matched the Python shaper, but the ESP32-S3's GDMA moves internal memory
at only 53-63 MB/s here, so it cannot replace the CPU for the bank fill: copying one 65,520-byte
bank takes 250k-306k cycles against an 85,608-cycle deadline. Gathering table rows is slower
still, and letting DMA copy the bank overlap while the CPU shapes the rest made every case
slower than the CPU alone. The CPU with the PIE vector unit stays the sample engine.

Firmware `dfd23f8`, ESP32-S3 at 240 MHz, radio off, `dmabench` three times (`runs.jsonl`;
analysis `python3 host/dmabench.py runs.jsonl`). Each run: four methods × 4 and 8 samples per
symbol × GDMA burst off/on, 184 bank checks, all equal to the independent Python model, guards
intact.

| Method, burst on | 4 samples/symbol, max cycles | 8 samples/symbol, max cycles | Effective rate |
|---|---:|---:|---:|
| Copy a whole precomputed bank | 254,940 | 251,962 | 63.4 MB/s |
| Gather one table row per symbol | 542,582 | 378,461 | 37.5-49.9 MB/s |
| DMA copies the overlap, CPU shapes the rest | 92,441 | 74,550 | |
| Same, pipelined | 74,631 | 71,195 | |

Burst off is 15-20% slower in every method. Against the CPU-only fill of the same banks, the
overlap methods' worst paired change was -72% to -156% (slower), so none meets the 5%
improvement criterion. Rates include interrupt latency. For scale, the CPU's own memcpy into
internal SRAM measured 384 MB/s (results/2026-10-16-memory).

What it means: the overlap reuse idea is right, but the copy has to be done by the CPU, which
is what exp/rd-fill's cached overlap does. The TRM survey of the other peripherals is in
ignored notes/rd-offload.md.
