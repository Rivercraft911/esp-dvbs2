# Packed encoder and cache placement

The coordinator measured **167,053/185,000 cycles/frame at four samples/symbol**
on 603833c, with live maxima of 170,026/187,413 cycles for 8PSK 2/3 and 5/6.
At eight samples/symbol (5 Msym/s), it regressed to 301,060/423,382 cycles/frame.
The follow-up restores main's sparse mapping path at 5 Msym/s and retains direct
packed mapping at four samples/symbol. Its timing is awaiting a coordinator run.
No board, serial port or radio was accessed during this follow-up.

The original projection below predates those measurements.
[Original offline checks and model records](../results/2026-10-16-max-encoder/README.md).

## 5 Msym/s follow-up

`55b868b` replaced main's sparse mapping/packing loop with the direct packed
mapper in every 8PSK stream. Its new table touches 8,192 B of data-cache lines;
the old sparse table touches only 512 B. The ring destination and mapping scratch
are internal, so these are not cold PSRAM output writes. The 603833c layout puts
the FEC workspace at `0x3c0c09b0`, with nibble BCH and no resident BCH table.
`24c7412` only changes that placement at four samples/symbol. The later commits
add allocation reporting and a live maximum, not new 5 Msym/s stage algorithms.

The linked line inventory, including that PSRAM address, gives:

| Known hot data-cache union | 2/3 | 5/6 |
|---|---:|---:|
| Direct packed mapper | 34,208 B | 35,360 B |
| Restored sparse mapper, fixed PL symbols still internal | 26,528 B | 27,680 B |

The difference is exactly `8192 - 512 = 7680 B`; the configured cache is 32,768 B.
The model's maximum set occupancy falls from 10/11 to 8/9 lines in an eight-way
cache. Measured 5 Msym/s BB/BCH/map flash misses rise to 121/61/106 per frame
for 2/3 and 213/66/208 for 5/6. Instruction misses total at most 19 per stage
over 6,760 frames. These observations support data-cache pressure as the cause,
rather than instruction fetches. This is source attribution and a working-set
model, not a silicon bisect. Data placement can still change conflicts.
Source: coordinator-supplied `notes/board-runs-603833c.jsonl`; main's reference
is [the nibble stream run](../results/2026-10-16-next/pace-nibble.jsonl).

`DVBS2_MAP_SPARSE` restores the former index, sparse lookup, in-place packing
and copy sequence using existing scratch. Packed PL framing remains prepared
once. No new heap, workspace, table or ring allocation is made. Context sizes
stay 15,184/16,528 B; the 12,420 B workspace stays in PSRAM at 5 Msym/s, and
the BCH cache stays at 0 B. The fallback control loop lives in flash: versus
603833c, linked internal code shrinks 64 B, static RAM is unchanged, flash code
grows 276 B and flash data grows 48 B. The direct PIE kernel is unchanged.

Using the coordinator's rounded main totals, the prediction is
`[262000, 290000] - 9800 + 1500 = [253700, 281700] cycles/frame`, about 254k/282k.
It restores main's measured mapping cost and substitutes the measured prepared
PL cost. Dispatch, instruction-cache and data-placement changes are not modelled;
only the board can establish the 2% requirement. Internal FEC plus the new table
at 5 Msym/s was rejected because of the Wi-Fi heap limit. There is no table
redesign or cache-size change. Reproduce the sparse inventory with:

```sh
python3 host/encoder_memory.py firmware/build/esp_dvbs2.elf \
  --workspace 0x3c0c09b0 --sparse-packed
```

Offline verification: 27 host scripts pass, including every C encoder stage
against Python, actual stream lifecycle/DAC words and allocation failure cleanup.
QEMU passes all 140 FEC CRCs, 14 PIE checks and 384 guarded complete packed frames
across both mapper/BCH selections, all six modes, both pilot settings and every
output alignment. QEMU timing is not used. ESP-IDF v6.1 builds cleanly.

## Working set

`host/encoder_memory.py` inventories linked symbol addresses and the cache lines
the stream can touch. It counts the old sparse mapper's 64 addressed words as
512 B of 32-byte lines, rather than its whole 4,096 B allocation. It includes the
bounded vector over-read at the end of the 8,100 B FEC buffer.

| Known hot cache-line union, normal frames with pilots | 2/3 | 5/6 |
|---|---:|---:|
| Baseline flash tables | 19,168 B | 20,288 B |
| Baseline with PSRAM FEC buffer | 27,296 B | 28,416 B |
| Candidate, internal FEC buffer and BCH table | 22,080 B | 23,232 B |
| Baseline maximum lines per modelled cache set | 9 | 10 |
| Candidate maximum lines per modelled cache set | 7 | 8 |

Capacity alone does not explain the baseline misses: both unions fit in 32 KiB.
The set calculation assumes `set = (address / 32) % 128`, from the configured
32 KiB, eight-way, 32-byte cache. It suggests conflicts at the linked addresses.
It is not a measured replacement trace. Baseline addresses come from an ea0958d
build; its illustrative PSRAM address, `0x3c0c09b0`, comes from the earlier
[paced records](../results/2026-10-16-next/pace-nibble.jsonl), not that exact ELF.
Sweeping the workspace's low 12 address bits in 16 B steps keeps the model's
baseline maximum at 9/10 lines per set. Small constants, interrupts and other
tasks are excluded. Rerun after every merge because link placement matters:

```sh
python3 host/encoder_memory.py firmware/build/esp_dvbs2.elf --resident-bch
```

The [ESP32-S3 TRM v1.8](https://www.espressif.com/sites/default/files/documentation/esp32-s3_technical_reference_manual_en.pdf),
4.3.3.2 p. 405, specifies shared instruction/data caches and external-memory
arbitration. The saved stream profiles show BB/PL rereads, not exclusive cache
stall cycles. Their counters cover both cores and interrupts; PSRAM counters
reading zero do not prove that PSRAM misses cost zero.

## What changes

At four samples/symbol, the stream puts its existing 12,420 B workspace in
internal RAM. Only 8,100 B hold FEC bits; 4,320 B belong to the plain reference
encoder's parity workspace, except for bounded fast-path over-reads. Retaining
that public layout avoids a second workspace API. A separate 8,192 B allocation
holds byte BCH rows, copied once after prepare. It survives every frame. The
scratch-copy BCH experiment had copied those rows every frame.

Eight-sample and QPSK streams retain their former workspace allocation and nibble
BCH selection. Direct packed mapping now applies only to four-sample streams;
eight-sample streams select the restored sparse path. A four-symbol
index has three bit planes, `P0' | P2<<4 | P1<<8`, where the first symbol is the
high bit of each nibble. A lookup returns four phase nibbles in a halfword:

```
packed4(x) = T[x & 0x7ff] XOR ((x >> 10) & 2)
```

Index bit 11 only toggles bit 1 of the first phase. Removing it halves a full
16 KiB table to 8 KiB. Eight word lookups and a vector unzip produce 32 packed
symbols directly at the ring destination. This removes phase-byte expansion,
the second packing pass and its temporary copies. Existing runtime LDXQ address
compensation is retained. Instruction semantics: TRM 1.8.37 p. 113,
1.8.56 p. 132 and 1.8.207 p. 290. `EE.VSR.32` is arithmetic, so both index paths
mask unwanted bits. All 4,096 possible indices have independent formula checks.

The 45-byte header and up to 396 bytes of packed pilots are prepared once in
the context. PL framing copies them each frame. Gold sequences, BB scrambling
sequences and dummy frames were already prepared once. The fill still consumes
the same packed bytes and unpacks them with its existing kernel.

## Cycles and RAM

Warm instruction traces retain register-window spills and exclude interrupts;
the pipeline model uses TRM 1.7.1/Table 1.7-2 pp. 65-69 and 1.7.3 p. 74. It omits
cache, PSRAM and SRAM contention. QEMU CCOUNT is not used as timing.

| Stage model, cycles/frame | Old 2/3 | New 2/3 | Old 5/6 | New 5/6 |
|---|---:|---:|---:|---:|
| BCH | 72,738 | 60,631 | 92,320 | 75,839 |
| Packed mapping | 44,975 | 21,343 | 44,975 | 21,343 |
| Packed PL framing | 2,454 | 957 | 2,454 | 957 |
| Whole encoder, excluding payload | 157,317 | 120,081 | 169,214 | 127,560 |

The stream projection scales measured BCH and map stages by those model ratios,
keeps measured payload and LDPC costs, assumes BB recovers to 8k/10k cycles and
PL to `3.1k * 957/2454 = 1.21k` cycles. The latter assumptions use the earlier
[warm framing measurements](../results/2026-10-16-buildout/README.md); their
recovery is unmeasured. The resulting totals are 191,251/213,949 cycles/frame.
Keeping the current slow BB/PL costs instead gives about 214,738/237,832 cycles,
so placement is essential to the 5/6 target. A 25% sensitivity allowance on the
central projection gives 239k/267k cycles, which fails the 226k target. This is
a candidate to measure, not a qualified 20 Msym/s transmitter.

| Internal allocation delta versus ea0958d | Bytes |
|---|---:|
| Workspace moved from PSRAM, four-sample streams | +12,420 |
| Resident byte BCH table, t = 10/12, four-sample streams | +8,192 |
| Context, all modes: packed fixed symbols and pointer fitting prior padding | +448 |
| Linked code in internal RAM | +164 |
| Static data and live-maximum counter, including link padding | +16 |
| Total, 2/3 and 5/6 at four samples/symbol | **+21,240** |

The counter's static cost and final link sizes are recorded in the offline run.
Requested heap bytes exclude allocator headers/alignment. Stream records print
requested and allocated sizes, addresses, free heap and largest block so the
board can account for those too. Expected internal contexts: 15,184 B for 2/3,
16,528 B for 5/6. Ring slots, payload, dummy and shaping table costs are unchanged.
The reference workspace remains PSRAM in other modes. Read-only data grows by
8,448 B and flash code by 436 B in the original ELF comparison. Apply the
follow-up deltas above for the current build.

Subtracting requested bytes and code from the earlier Wi-Fi-up free heap predicts
about 34.0/32.7 KB free for four-sample 2/3 and 5/6, and about 32.1 KB in the
tightest eight-sample mode (1 KB = 1,000 B). Allocation overhead and Wi-Fi startup
remain unverified. Actual free heap, largest block and Wi-Fi allocations must be
checked by the coordinator before adoption.

## Other choices and 170k

A raw phase byte per symbol adds 33,312 B for three guarded ring slots. One
16-bit three-symbol window index per symbol adds 99,888 B; the fill could reuse
delayed indices for the other windows, but this still exceeds the available RAM.
Pre-split offsets grow similarly. Neither representation is implemented. The
current fill unpack is already vectorized; changing it belongs to the fill front.
A 64 KiB data cache consumes another 32,768 B of SRAM (TRM 4.3.2, item 5, p. 404), more
than the chosen placement. A full 16 KiB mapping table would save four instructions
per 32 symbols but double its cache footprint; the 8 KiB factorization was used.

At 26.67 Msym/s, `240e6 cycles/s * 22194 symbols / (80e6/3 symbols/s)` gives
199,746 cycles/frame, or 169,784 with 15% reserve. Replacing synthetic payload
generation (43,527/54,386 cycles) with DMA ingress using at most 10,000 CPU
cycles/frame projects 157,724/169,562 cycles. An internal payload copy alone
would cost about 3,356/4,200 cycles at the measured 384 MB/s; this is an estimate,
not an ingress benchmark. Required payload ingress is 51.6/64.6 Mb/s, before
transport overhead. USB's measured 4.6 Mb/s is insufficient. SPI DMA completion,
buffer handling and bus contention must be measured. That is the next experiment.

## Coordinator runs, radio off

Build/flash through the coordinator's normal workflow. Keep the archived
`N0CALL` settings. Run these in order, using a fresh output directory. None is
executed by this agent. Each pace is 30 s plus identification samples.

```sh
OUT=notes/max-encoder-5m-check
mkdir "$OUT"
python3 host/board.py 'dvbpace 13 5m 30' --timeout 60 --out "$OUT/runs.jsonl"
python3 host/board.py 'dvbpace 15 5m 30' --timeout 60 --out "$OUT/runs.jsonl"
python3 host/board.py 'dvbpace 13 10m 30' --timeout 60 --out "$OUT/runs.jsonl"
python3 host/board.py 'dvbpace 15 10m 30' --timeout 60 --out "$OUT/runs.jsonl"
python3 host/board.py 'dvbpace 4 30' --timeout 60 --out "$OUT/runs.jsonl"
python3 host/board.py 'fecbench pie' --timeout 60 --out "$OUT/fec.jsonl"
python3 tests/test_fecbench.py "$OUT/fec.jsonl" --correctness-only
python3 host/board.py 'dvbdump 4 40' --timeout 90 --save "$OUT/4.iq" --out "$OUT/runs.jsonl"                    # CRC 1435641601
python3 host/board.py 'dvbdump 13 5m 40 128 1' --timeout 90 --save "$OUT/13-5m.iq" --out "$OUT/runs.jsonl"    # CRC 1937985440
python3 host/board.py 'dvbdump 15 5m 40 128 1' --timeout 90 --save "$OUT/15-5m.iq" --out "$OUT/runs.jsonl"    # CRC 1163322913
python3 host/board.py 'dvbdump 13 10m 40 128 1' --timeout 90 --save "$OUT/13-10m.iq" --out "$OUT/runs.jsonl"  # CRC 474263962
python3 host/board.py 'dvbdump 15 10m 40 128 1' --timeout 90 --save "$OUT/15-10m.iq" --out "$OUT/runs.jsonl"  # CRC 1757765864
```

Pass criteria, fixed before the run:

| Pace | Maximum mean, cycles/frame | Maximum live, cycles/frame |
|---|---:|---:|
| 13 5m | 267,240 | 267,240 |
| 15 5m | 295,800 | 295,800 |
| 13 10m | 170,394 | 173,426 |
| 15 10m | 188,700 | 191,161 |
| 4 | 699,685 | record |

The 5m mean ceilings are `1.02 * [262000, 290000]`, the coordinator's rounded
main baselines. The live ceilings also reject occasional live regressions;
main did not record a separate live maximum. The 10m ceilings are 2% above
603833c's measured means/live maxima, rounded down. Keep the all-frame maximum
and prefill count in the record.

Require `ESP_OK`, no encoder failure, zero underruns, zero `late_fills`, zero
`fatal_late` and positive fill slack. Fill maximum must stay below 45,200 cycles
at 5m, 60,000 at 10m and 43,457 for QPSK. Small nonfatal `late_switches` are
present in the baseline and are not fill failures.

At 5m require `packed_mapper=sparse`, PSRAM workspace, nibble BCH, 0 B BCH cache,
and unchanged requested/allocated context, workspace and ring sizes versus
603833c. No additional internal heap is allowed. At 10m require
`packed_mapper=direct`, internal workspace and an 8,192 B resident BCH table.
Expect BB/BCH/map misses to return toward main's 63/28/35 per frame for 2/3;
record stage means and misses for both rates even if timing passes. A timing
failure or persistently elevated misses leaves the fix unqualified.

Require all 140 FEC CRCs, all 14 zero-mismatch PIE checks, all five dump CRCs
above and matching overlap comparisons. Decode all five with `host/onair.py`,
the supplied LeanDVB executable, `--stream 40 --callsign N0CALL --mer --shift 0`;
use `--rate 4000000` for QPSK and `--rate 40000000` for 8PSK. Require at least
30 contiguous bit-exact frames, zero LDPC flips and zero BCH failures.
