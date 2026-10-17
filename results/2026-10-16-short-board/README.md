# The 6-symbol pulse on the board, radio off

The chip's own 26.67 Msym/s samples (8PSK 2/3, 3 samples/symbol at 80 MS/s, 51.6 Mb/s) decode
bit-exact: LeanDVB recovered 12 contiguous frames, 515,520 payload bits, with zero LDPC flips,
zero BCH failures and 40.5 dB MER against the known symbols (host model 40.6 dB). The dump CRC
equals the host model's. But the 3 samples/symbol kernel takes 66.5k cycles per bank against
the 42,804 available at 80 MS/s, so 26.67 Msym/s does not run in real time on this design. The
dump runs slower than real time and proves only the samples.

Firmware `fdd68dc` (exp/rd-short, based on exp/rd-max, before the build-out), ESP32-S3 at
240 MHz. Commands from `docs/short-pulse.md` on exp/rd-short; records in `runs.jsonl`, `fec.jsonl` and
`decode-26m667.json`. Nothing was transmitted.

## Kernels, `membench`

All 30 table and output checks match the Python model (`host/shaper.py verify`).

| Kernel, 6-symbol pulse, PIE | Cycles/sample | One bank | Today's 9-symbol kernel |
|---|---:|---:|---:|
| 3 samples/symbol (`short3`) | 4.06 | 66.5k cycles, 277 µs | none |
| 4 (`short4`) | 2.30 | 37.6k, 157 µs | about 2.3 |
| 8 (`short8`) | 1.55 | 25.4k, 106 µs | 1.78 |
| 40 (`short40`) | 1.34 | 22.0k, 92 µs | 1.75 |

The `short3` result is the doc's predicted refutation (about 66,600 cycles): at 3 samples per
symbol the pulse does not fall on 4-word vector boundaries, so the kernel pays per sample what
the others pay per vector. Where the samples per symbol are a multiple of 4, the shorter pulse
is 13-23% faster than today's. The first `membench` record is "internal allocation failed": one
memory test could not get its buffer on this older base; the shaper tests all ran.

## Paced runs

| Run | Result | Longest fill | Smallest slack | Encoder mean / max |
|---|---|---:|---:|---:|
| `dvbpace 13 13m333 60 128 0 span6` (40 MS/s, 3 sps) | ESP_OK, 0 late, 0 underruns | 80,597 | 4,743 of 85,608 | 228.9k / 319.4k |
| `dvbpace 13 26m667 1 128 0 span6` (80 MS/s) | ESP_ERR_TIMEOUT after 3 fills, 1 fatal | 67,333 | | 287.0k / 333.5k |

13.33 Msym/s (25.8 Mb/s at 8PSK 2/3) holds its deadlines at 40 MS/s, with 5.5% slack, on a base
that lacks the build-out's packed frames and overlap cache. `fecbench hot`: slowest 8PSK 2/3
frame 228,616 cycles.

## Not shown

- No RF, and no real-time 26.67 Msym/s.
- The 13.33 Msym/s run has not been repeated on main's faster stream.
