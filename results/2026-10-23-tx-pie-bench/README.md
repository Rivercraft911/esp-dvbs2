# Transmitter PIE kernels on the board

**Result.** On the board, every covered PIE kernel matched its reference in every
configuration the transmitter uses: 82,493 counted cases plus the reused host checks. One
configuration that the transmitter does not use did not match. With
short 8PSK frames, the fast PIE mapper gets the last 12 to 40 symbols of each frame wrong.
That affects the phase output in every variant and the packed output without `MAP_SPARSE`.
BB, BCH and LDPC were exact in those frames. The transmitter sends short 8PSK only at
5 Msym/s, through the sparse packed path, and that path was exact. Its pinned dumps also
match. No BB input was read outside the 16-byte blocks that hold it: 22,196 inputs were
watched by the debug assistant's region monitor.

Bench: [`tests/tx_pie_bench.c`](../../tests/tx_pie_bench.c), firmware 0528ec7, on the XIAO
ESP32-S3 (44:B1:76:B0:E5:8C), radio off. One run took 89 s. Record: `runs.jsonl`.

## Why a board bench

Host builds define neither `DVBS2_PIE` nor `__XTENSA__`, so the host tests run portable C
wherever the firmware runs assembly. Codex review item 4 found that `test_qpsk_axes.py`'s
"C/PIE" loop ran C twice. On the board, every check below calls the real kernels. Each one is
compared with portable C from the same build or with an independent oracle.

## Parts

| Part | Kernels | Reference | Cases | Mismatches |
|---|---|---|---:|---:|
| monitor | region monitor itself | known reads | 5 | 0 |
| stages | all of `pie.S`, `ts_pie.S`, `qpsk_signs.S` reached by the fast stages | plain encoder (`dvbs2.c`) | 2,324 frames | 714, all short 8PSK |
| bb_inputs | `dvbs2_pie_xor`, `dvbs2_pie_crc8`, `dvbs2_pie_crc8_packet` | plain BB | 19,872 | 0 |
| xorshift | `payload_xorshift8` | scalar xorshift32 | 120 | 0 |
| fills | shaper fill4/8/40, raw 40, QPSK radix-4, short pulse, QPSK quads, word copy | `shaper_fill_c`, `shaper_qpsk_c`, word loop | 15,904 | 0 |
| ring | packed unpack (raw, codes, QPSK), two-bit copies | scalar models | 44,268 | 0 |
| qpsk_axes | axis queue, `machine_axis_render_pie` | plain encoder, host oracles | `tests/qpsk_axes.c` | 0 |
| shaper_axes | `shaper_axes8_pie` | full-table oracle | `tests/shaper_axes.c` | 0 |
| psk8_kernels | lofty prepare and render, lofty mapper | independent references | 4,320 slices, 36 frames | 0 |

**Stages.** Covers all 14 normal and 12 short MODCODs with pilots on and off. Each mode runs
four GCS payloads (full, empty, 13 bits, three bits short of full) and three consecutive TS
frames. Each frame runs through every variant the mode offers:

| Variant | Frames |
|---|---:|
| Portable C | 364 |
| Flash tables | 364 |
| Nibble BCH | 364 |
| Resident tables | 364 |
| Split-pair BCH | 210 |
| Resident byte BCH | 308 |
| Unrolled LDPC pack | 84 |
| `MAP_SPARSE` (8PSK) or `MAP_QUADS` (normal QPSK) | 266 |

BCH ran as pair 196 times, split-pair 210, byte 1,190 and nibble 364. Input and output
addresses moved through all 16 alignments. Guards were checked around the workspace tail,
context, tables, phase output and packed output.

**BB inputs.** QPSK 1/2, QPSK 9/10, 8PSK 2/3 and 8PSK 9/10 normal frames, plus QPSK 1/2 and
QPSK 8/9 short frames, at all 16 input offsets. GCS used 19 lengths around byte, word and
vector edges. TS started from each of the 188 packet phases, with arbitrary running CRCs.

**Read monitor.** Area 0 of the core-0 DRAM monitor covers the 256 bytes above the
input's last 16-byte block. Area 1 covers the 256 bytes below its first block. Only reads are
enabled, and the raw status is polled. The controls show that it catches a scalar read just
past each end and a `dvbs2_pie_crc8_packet` vector load into either area. It stays quiet for
224 bytes read inside. When the input ends on a block boundary, the check is exact. Otherwise
it allows the rest of the last block. A 16-byte block never crosses a memory-region boundary.

## The short 8PSK mismatch

All 714 mismatches are short 8PSK frames, MODCOD 12 to 16, with both pilot settings:

- Phase output: 392 mismatches, in every PIE variant including `MAP_SPARSE`.
- Packed output: 322 mismatches, in the variants without `MAP_SPARSE`.
- Portable C was exact.

The first wrong symbol lies 12 to 40 symbols before the frame end. The value in that range
depends on the output alignment. A short 8PSK frame has 5,400 data symbols. The last
1,440-symbol run (pilots on) or the whole frame (pilots off) is 8 symbols longer than a whole
number of 16-symbol vectors. `map_pie` in `fast.c` finishes such runs with a loop written for
QPSK: "Short QPSK runs end 4 or 8 symbols past a whole vector". Normal 8PSK runs are always
whole vectors. This is the likely cause, but it was not traced further.

The transmitter cannot reach this path. Short frames are offered only at 1 Msym/s QPSK and
at 5 Msym/s (`dvbtx.c`). At 5 Msym/s, 8PSK is prepared with `MAP_SPARSE` and encodes into the
packed ring. That case was exact here, and the pinned short dumps (`tests/data/short_crcs.json`)
match on the board.

## Not covered here

`station_repeat40` (the CW ID's 40-sample copy) is checked only through the pinned ID dumps.
The bench measures correctness, not speed. `fecbench` and the paces time the kernels.

## Running it

```sh
(. ~/.espressif/tools/activate_idf_v6.1.sh && cd tests/tx_pie_bench && idf.py build flash -p PORT)
python3 host/board.py check --reset --timeout 600 --port PORT --out runs.jsonl
```

Mismatch records give a stage case as `[1000 * short + 10 * MODCOD + payload case, variant,
pilots, first wrong byte]`. Variants are numbered in the table order above, from 0 (portable C)
to 7 (`MAP_SPARSE` or `MAP_QUADS`). Only the first three of each kind are printed; each part's
record counts all of them.
