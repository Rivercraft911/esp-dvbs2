# Board self-check after run memory and the 8PSK merge

**Result.** All 30 checks of [examples/self-check](../../examples/self-check/README.md) passed
on firmware 27259c5. That is 7 golden dumps, each decoded exactly by LeanDVB, 12 paces, the
VCM dump and decode, and no heap call after boot. 8PSK 9/10 at 26.67 Msym/s (17/3/80/16)
matched its golden CRC again, now that the dumps wait for whole frames.

The firmware reports `27259c5-dirty`. The only uncommitted edits were the new self-check
gates in `examples/self-check/check.py` and its README, committed right after this run. The
board image is the code of 27259c5.

The gates: every pace keeps its deadlines, with each run-memory arena at its documented size
and within it. The heap is checked once, at the end: `info` reports 0 allocations and 0 frees
after boot. The 8PSK 26.67 Msym/s ceilings come from the keyed real-payload runs of
[tx-table-8psk](../2026-10-23-tx-table-8psk/README.md) plus 4,500 fill and 10,000 encoder
cycles for profiling. The paces came in under those keyed means plus 300 cycles:

| Pace (10 s) | Encoder mean | Ceiling | Slowest fill | Ceiling | Steady dummies |
|---|---:|---:|---:|---:|---:|
| 8PSK 2/3 | 159,389 | 169,352 | 37,499 | 42,147 | 0 |
| 8PSK 5/6 | 166,773 | 176,803 | 37,721 | 42,357 | 0 |
| 8PSK 9/10 | 157,242 | 166,956 | 37,544 | 42,045 | 0 |

`runs.jsonl` holds every command record and `summary.tsv` the verdicts. Dump IQ and receiver
logs were left out of the repository.
