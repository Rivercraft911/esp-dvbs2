# Packed encoder, offline checks

No board, serial port or radio was accessed. There are **no new silicon timing
results** here. Sources: ea0958d baseline, candidate f989508 on exp/max-encoder.
`runs.jsonl` records the linked working sets, instruction models, projected
stream costs, size changes and correctness counts. No QEMU CCOUNT is treated as
timing. The candidate has not met the live-stream target on hardware yet.

| Normal 8PSK frame with pilots | 2/3 | 5/6 |
|---|---:|---:|
| Measured old stream, supplied baseline | 262k cycles | 291k cycles |
| Old packed instruction model, no payload | 157,317 cycles | 169,214 cycles |
| New packed instruction model, no payload | 120,081 cycles | 127,560 cycles |
| Model-assisted stream projection, including payload | 191,251 cycles | 213,949 cycles |
| Projection with 25% sensitivity allowance | 239k cycles | 267k cycles |

The central projection assumes recovery of the prior warm BB/PL framing costs.
It is not a bound. The fill implementation is untouched, and its measured
57.2k-cycle maximum has not been remeasured. Full method, assumptions, byte
accounting and coordinator commands are in [docs/max-encoder.md](../../docs/max-encoder.md).

Verification: all 27 host test scripts pass, including the new ELF inventory
test. The actual stream lifecycle passes eight modes, two 16-frame dumps each,
every DAC word against the plain encoder and every allocation-failure cleanup.
QEMU passes all 140 `fecbench pie` CRCs and 14 zero-mismatch stage checks, plus
192 packed frames covering six modes, both pilot settings, all 16 alignments and
resident byte BCH. ESP-IDF v6.1 builds without warnings. The checked-in
[QEMU harness](../../tests/encoder_pie/README.md) reproduces those semantic checks.

The two traced apps, model tools, raw traces, build logs and serial logs remain
in ignored `notes/encoder-model/` and `notes/encoder-*.log`. Trace/log hashes and
the final ELF hash are in `runs.jsonl`. Those traces use a prior trace parser
and issue model from the same repository's encoder experiment, with packed
stage calls and explicit resident-table preparation added. They exclude
interrupts and include register-window spills. See the model's TRM references
and limitations in the design note.

Main firmware merge surface: `firmware/components/dvbs2/{fast.c,fast_tables.h,
pie.S,include/dvbs2.h}` and encoder allocation/profiling in `firmware/main/dvbtx.c`.
No fill, ring, radio, sample-glue or SDK configuration source changed. Host
changes are the fast-table generator and new memory inventory tool. Tests are
the C/table/stream checks and the new encoder QEMU harness. The complete branch
file list is `git diff --name-only ea0958d..exp/max-encoder`.
