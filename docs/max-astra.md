# 26.67 Msym/s: the combined budget fails

The four-symbol prototype reduces the estimated n3 bank fill from 60.7k to **43.1k
cycles**, but does not support 26.67 Msym/s. Together with the measured 262.1k-cycle
encoder it needs **463.2k cycles/frame**, against **399,492 across both cores**.
It is a `membench` experiment, not an enabled stream mode. No board or radio was used.

At three samples/symbol the fastest clock choice this study predicts will fit is
**40 MS/s / 3 = 13.333 Msym/s, 25.809 Mb/s**. That is a candidate, not a demonstrated
end-to-end rate. The measured complete transmitter remains at 10 Msym/s. This study
does not rule out 20 Msym/s at four samples/symbol, or a different 26.67 Msym/s design.

## What changed

`exp/rd-short`'s six-symbol inverse-hold pulse is retained. Four output symbols need
nine input symbols, at positions -5 through 3 relative to the first output symbol.
Split those into triples `[-5,-4,-3]`, `[-2,-1,0]`, `[1,2,3]`. Each table entry now
contains all twelve output samples contributed by its triple:

```
Y[3*j+p] = sum_g round(sum_(k in g) L[s[k]] * h[3*(j-k)+p] / 2^38)
j = 0..3; p = 0..2; h[a] = 0 outside 0..17
L = integer constellation coordinate in 2^-30; h = tap in 1/256 DAC LSB
```

Each table has 512 rows of 48 B: three tables total **73,728 B**. Three aligned
vectors are emitted per block, with no output packing. The input is four phase
codes in a 16-bit word, newest code in bits 2:0. Its size is still 0.5 B/symbol.
The prototype assumes this format is already available. Its encoder conversion,
frame-boundary stitching and stream scheduler are **not implemented or timed**.
Normal frames have 22,194 symbols, so a block straddles every other frame boundary.

Regrouping changes table rounding. Three complex roundings have error at most
`3/sqrt(2) = 2.122 LSB`; reserve 3 LSB from a peak of 128 LSB. Symmetric rounding
preserves zero mean. Tests exhaust each table's fields and their independent extrema,
prove no I-to-Q carry, and compare C and PIE output with Python. Legacy shapers,
encoders, rate parsing and stream code are unchanged.

## Cycles and the limit of the result

The model replays the assembled kernel with the existing single-issue/interlock
model: [ESP32-S3 TRM v1.8](https://www.espressif.com/sites/default/files/documentation/esp32-s3_technical_reference_manual_en.pdf),
1.7.1 pp. 65-66 and Table 1.7-2 pp. 66-73. Vector loads, fused add/store and aligned
stores follow 1.8.88 p. 164, 1.8.75 p. 151 and 1.8.192 p. 275. This is not QEMU timing.
Cache/bus stalls, interrupts and call/register-window redirects are excluded.

The steady loop issues **34 instructions per four symbols**: 15 input/index/address
instructions, 18 vector instructions, one input-pointer increment. A hardware loop
removes the decrement and taken branch. The full 16,380-word kernel models at
**46,427 cycles**, including startup. The fill estimate uses rd-short's assumptions:

```
Cfill = 46427 * 14268/16380 + (2112 + 2176)*0.5 + 500 = 43084.8 cycles
```

The 0.5 cycles/word copies and 500-cycle control allowance are assumptions, not
measurements. No full-bank implementation is claimed by that equation. Multiplying
the whole estimate by 1.25 gives 53,856 cycles; this is a sensitivity case based on
past model error, not a guaranteed upper bound.

The bank/frame geometry comes from `radio_s3.h`, and DVB-S2 normal-frame geometry
from EN 302 307-1 clauses 5.3 and 5.5.2-3:

```
Nframe = 90 + 64800/3 + 14*36 = 22194 symbols
Cframe = 240e6 * Nframe / (80e6/3) = 199746 cycles/core
Cbank = 240e6 * 14268 / 80e6 = 42804 cycles
Rpayload = (43040-80) * (80e6/3) / Nframe = 51.617554 Mb/s
occupied bandwidth = (1+0.35) * (80e6/3) = 36 MHz
Wframe = Cencoder + Cfill * (3*Nframe/14268)
```

The encoder input is the rounded measured mean from
[pace-nibble](../results/2026-10-16-next/README.md), including payload generation.

| n3 candidate | Fill cycles/bank | Shaping cycles/frame | Encoder cycles/frame | Total cycles/frame |
|---|---:|---:|---:|---:|
| rd-short model | 60,700 | 283,258 | 262,100 | 545,358 |
| four-symbol model | 43,085 | 201,056 | 262,100 | 463,156 |
| four-symbol, 25% fill guard | 53,856 | 251,320 | 262,100 | 513,420 |

The two-core limit is 399,492 cycles/frame, or **339,568 with 15% reserve**.
Even deleting all glue, copies, stalls and startup leaves
`34/4 * 22194 + 262100 = 450749 cycles/frame`. Thus rescheduling this kernel and
the present encoder cannot fit. This is a conditional instruction-count argument,
not a proof that every algorithm on the S3 must fail. Changing encoder placement
could change its measured cost; it must be measured again.

With the current encoder, 15% combined reserve permits only **16,601 fill
cycles/bank**. With this new fill, the encoder would need **138,512 cycles/frame**.
The free-scheduling break-even rate is 23.001 Msym/s, or 19.551 Msym/s with 15%
reserve, but neither is an available n3 rate at the specified 40/80 MS/s clocks.
At 13.333 Msym/s the 25%-guard fill and measured encoder separately leave 37.1%
and 34.4% of their respective deadlines.

Shrinking overlap cannot supply the missing 15% margin. Even with no overlap,
the kernel alone needs `34/12 = 2.833 cycles/sample`, above the 80 MS/s target
`0.85 * 240/80 = 2.55 cycles/sample`. Splitting BCH, LDPC or PL framing between
cores does not remove their cycles. Pre-emitting three 16-bit offsets per block
triples the ring to 1.5 B/symbol and moves index work onto the already-late encoder.
A full six-symbol LUT needs `8^6 * 3 * 4 = 3,145,728 B`; PSRAM cannot supply the
320 MB/s output stream at the measured 59 MB/s read rate.

## Memory and signal evidence

This branch adds 248 B IRAM text plus 8 B alignment, **256 B internal capacity**
in total; DRAM data/BSS are unchanged. The linked image adds 1,300 B flash text
and 240 B flash rodata. These are ELF section differences against `ea0958d`, not
heap measurements. The builder uses a 432 B stack frame; the PIE kernel uses 32 B.

During `membench` only: 49,152 B of the existing 65,536 B shaper store hold two
tables; a third table allocates 24,576 B, and packed input allocates 2,734 B.
The **27,310 B temporary heap request** excludes allocator overhead; both blocks
are freed. The banks and shaper store are reused. No new persistent sample buffers.
The 72 KiB design would displace packed frames from the table store if integrated;
it has no qualified Wi-Fi memory layout. Do not subtract its temporary benchmark
allocation from the unrelated measured 32.7 KB streaming-mode heap and call it a fit.

All 27 host scripts pass, including all 140 FEC CRCs and existing stream sample
comparisons. QEMU passes the existing 324 shaper cases, 1,520 sample-glue cases,
13,058 pack/unpack cases and 24 new block cases. Both IDF builds are clean.

Four independent LeanDVB host loopbacks pass: rd-short and block4, MODCODs 13 and
15, sixteen input frames each. Each recovers twelve contiguous frames with zero
payload errors, LDPC flips/checks and BCH failures. The rd-short IQ CRCs are
339035381 and 3535002931, matching that branch. Block4 CRCs are 2269685677 and
631214921. Known-symbol MER is 40.57/40.58 dB for block4, **without DAC hold**.
The linear taps retain +0.230 dB model mask margin; the regrouped quantized table's
complete mask and real analog spectrum are unqualified. Decode is not a mask test.

## Starting today

I would keep the four-sample representation and first qualify 20 Msym/s with the
16 KiB short-pulse table. Use the freed SRAM for the measured hot encoder working
set, keep packed frames internal, and make bank production a contiguous operation.
Publish one complete frame at a time; precompute fixed headers, pilots and scrambling
only where profiling shows a benefit. Keep FEC on core 0 and filling on core 1 until
both fit, then consider moving coarse jobs into measured slack.

For a later 26.67 Msym/s design, require a whole fill below **36,383 cycles** and
a whole encoder below **169,784 cycles**, both including ingress/publication work,
before adding a scheduler. The next experiment I would run is encoder SRAM
placement, then remeasure both cores concurrently. A warm isolated encoder number
is insufficient. The prototype's three table reads plus CPU stores and DAC reads
imply **1.60 GB/s aggregate internal traffic at 80 MS/s**. Whether separate SRAM
banks sustain it is unknown; the 384 MB/s memcpy result is not a bus-wide bound.

## Coordinator, radio off

On this branch, after the coordinator's normal build/flash, run these in order.
Each device command is intended to complete in about one minute or less.

```sh
OUT=results/<new-date>-max-astra-board
mkdir "$OUT"
python3 host/board.py membench --reset --timeout 75 --out "$OUT/mem.jsonl"
python3 host/shaper.py verify "$OUT/mem.jsonl"
python3 tests/test_maxrate.py --membench "$OUT/mem.jsonl"
python3 host/board.py 'fecbench nibble' --timeout 75 --out "$OUT/fec.jsonl"
python3 tests/test_fecbench.py "$OUT/fec.jsonl" --correctness-only
python3 host/board.py 'dvbpace 13 10m 30' --timeout 60 --out "$OUT/pace.jsonl"
```

Predeclared pass: all five block checks match, no allocation failure, all 140 FEC
CRCs and fourteen PIE checks match, pace reports `ESP_OK`, zero encoder failures,
underruns, late fills and fatal late events; maximum fill <=72,766 cycles and minimum
slack >=12,842 cycles. Compare the isolated `block4_pie` minimum with the 46,427-cycle
model and record median too. Even <=38,733 cycles only clears the *modelled fill*
15% gate; it does not clear the encoder or prove a streaming implementation.

Then verify the five unchanged dumps, each `--timeout 75`, saving IQ and JSONL:
`dvbdump 4 40` (1435641601), `dvbdump 13 5m 40 128 1` (1937985440),
`dvbdump 15 5m 40 128 1` (1163322913), `dvbdump 13 10m 40 128 1` (474263962),
`dvbdump 15 10m 40 128 1` (1757765864). Require USB CRC and zero overlap mismatches.
These test this branch's unchanged stream, not block4.

The n3 complete-stream candidate remains on `exp/rd-short` at `fdd68dc`:
`dvbpace 13 13m333 30 128 0 span6`, then
`dvbdump 13 26m667 16 128 1 0 span6`. Require the same 40 MS/s pace limits and dump
CRC 339035381, then LeanDVB recovery of >=10 contiguous frames with zero errors.
These commands are **not accepted by this branch**. Do not run a 26m667 pace expecting
success. Record the exact revision for each build; no keyed command is part of this plan.

Reproduce host models/decodes with `python3 host/maxrate.py --disassembly notes/block.dis
--receiver /path/to/leandvb`. Generate disassembly using `xtensa-esp32s3-elf-objdump -d
--no-show-raw-insn tests/shaper_pie/build/esp-idf/main/libmain.a`. The existing
[QEMU procedure](../tests/shaper_pie/README.md) also runs the new cases; check its log
with `python3 tests/test_maxrate.py notes/qemu.log`.
