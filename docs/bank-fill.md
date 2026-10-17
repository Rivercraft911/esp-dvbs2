# Bank fill, radio off

This candidate removes repeated overlap shaping and the large symbol-cache shifts. It does
**not establish the 36,000-cycle target**. The instruction model predicts a worst steady bank
of 39,018 cycles at 4 samples/symbol, or 41,366 cycles with the previously measured maximum
notification cost. At 8 samples/symbol it predicts 31,756 or 34,104 cycles, respectively.
The encoder is unchanged; its measured 262k cycles/frame remains the integration baseline.
No board, serial port, Wi-Fi or radio was accessed in this experiment.

## What the old profile actually says

The brief's 24k-cycle gap mixes two different paths. At base `ea0958d`, `fill` shapes all
16,380 bank words, including the overlap. `ring_cache` retains **symbols**, not samples.
The 4-sps raw-phase kernel also shifts every symbol before indexing. The assembled issue
model gives 41,677 cycles for 16,372 words (2.546 cycles/sample); its already-scaled sibling
gives 37,576 (2.295 cycles/sample). The latter is the approximately 2.3-cycle kernel in the
brief. Multiplying 14,268 fresh samples by 2.3 understates main's actual shaping work.

The [buildout profile](../results/2026-10-16-buildout/runs.jsonl) measured 42,998 shaping
cycles per equivalent full bank, plus 4,796 copying/ring cycles and 205 other cycles on
average at 4 sps. These are averages, not components of its 57,164-cycle maximum.
That maximum began at sample 2,498,769,044, whose physical offset is 44 words. Since no
callback exceeded 48,703 cycles, the other callback and scheduler glue account for **at
least 8,461 cycles** in that bank. The corresponding four saved 4-sps runs in buildout/next
have second pieces of 20, 44, 68 or 236 words and gaps of 8,294 to 8,714 cycles.

`ring_window` shifts up to 554 symbol bytes with `memmove` when that short second piece
extends its cache. The records do not time `memmove` separately, so attributing the whole
8.5k to it remains a hypothesis. The new profile splits `copy`, `whole`, `partial`, `save`
and `notify`, and reports `shaped_words` and `reused_words`. `worst_call` still describes
one callback, not necessarily the scheduler's worst complete bank. Cache counters still
observe both cores; interrupts are included in CCOUNT.

## Changes

The packed frames stay unchanged. A second unpack entry point emits phase codes `16*k`
directly: shift the low nibbles up and retain the high nibbles. It uses the same nine loop
instructions as raw unpacking. The existing scaled kernels then avoid one scalar shift per
symbol. The raw entry points remain for their existing callers and tests.

A private circular cache retains 2,176 finished DAC words. It covers the normal 2,112-word
overlap and the first bank's extra 64 words. The fill copies reused words, reads only the
fresh suffix from the ring, shapes it and saves its tail. It never reads the active physical
DAC bank. A wholly cached callback does not touch the ring. A short fresh callback appends
without moving previous samples. Fresh ring reads are monotonic, so their symbol cache now
needs just eight history symbols and two end-cut symbols, ten bytes in total.

The scheduler still splits contiguous output at the physical bank wrap. This preserves its
callback contract, including station-ID boundaries. Both pieces may save a tail, up to
4,352 words per bank. A literal single callback would need a wrapping-output interface
through the station-ID multiplexer and every radio source. That broader change was rejected
here. The costly 554-byte shift is removed without that interface change; any remaining
symbol-cache shift is bounded by ten bytes. Staging row pointers on this core was also
rejected: the existing model adds about eight producer instructions per symbol to save
about three consumer instructions. Shortening the pulse belongs to the separate pulse front.

## Budget and model

From `radio_s3.h`, with CPU frequency 240 MHz:

```
new words = 16,380 - 64 - 2,048 = 14,268 samples/bank
period = 14,268 samples / (80e6 samples/s) = 178.350 us
deadline = 178.350 us * 240 cycles/us = 42,804 cycles
15% reserve ceiling = 42,804 * 0.85 = 36,383.4 cycles
requested target = 36,000 cycles
cache capacity = (2,112 + 64) samples * 4 bytes/sample = 8,704 bytes
```

`host/fill_cycles.py` checks every reachable output-wrap phase, retains kernel prologues and
PIE tails, and accounts for both cache saves. `tests/test_fill_cycles.py` checks its geometry.
The issue/interlock rules come from the [ESP32-S3 TRM v1.8](https://www.espressif.com/sites/default/files/documentation/esp32-s3_technical_reference_manual_en.pdf),
1.7.1 pp. 65-66 and Table 1.7-2 pp. 68-69. Unpack uses 1.8.186 p. 268 and 1.8.212 p. 295.
Aligned copy uses 1.8.88 p. 164 and 1.8.192 p. 275: two vector loads and two vector stores
per eight words, modelled at 0.5 cycles/word. This is an instruction model, not a measured
SRAM bandwidth. Kernel fused operations are 1.8.74-75 pp. 150-151.

| Case | Kernel | Unpack | Cache copies | Assumed glue | Total | With 2,348-cycle wake |
|---|---:|---:|---:|---:|---:|---:|
| 4 sps, worst steady | 33,223 | 1,563 | 3,232 | 1,000 | 39,018 | 41,366 |
| 4 sps, initial bank | 37,712 | 1,568 | 1,088 | 1,000 | 41,368 | 41,368 |
| 8 sps, worst steady | 26,577 | 947 | 3,232 | 1,000 | 31,756 | 34,104 |
| 8 sps, initial bank | 29,179 | 864 | 1,088 | 1,000 | 32,131 | 32,131 |

All entries are **estimated cycles**. A 160-cycle unpack-edge allowance per fresh callback,
100 cycles per C tail symbol at 8 sps, and 32 cycles per partial-symbol copy are assumptions.
The 2,348-cycle wake is the old board maximum from
[buildout](../results/2026-10-16-buildout/README.md), not a bound. The initial bank has no
reusable samples and is primed before playback, but remains included in the scheduler's
maximum. It cannot release a 22,194-symbol frame and therefore has no notification cost.
A 25% stress factor before adding the wake gives 51,121 cycles for steady 4 sps
and 42,043 at 8 sps. Historical model error motivates this sensitivity check; it is not a bound.
The current 4-sps estimate fails the requested margin even before this stress factor.

Reproduce after an IDF build:

```sh
xtensa-esp32s3-elf-objdump -d --no-show-raw-insn \
  firmware/build/esp-idf/main/libmain.a > notes/fill.dis
python3 host/fill_cycles.py notes/fill.dis
```

Same-toolchain builds against `ea0958d`, IDF v6.1, default configuration:

| Resource | Base | Candidate | Change |
|---|---:|---:|---:|
| DIRAM data | 26,776 B | 35,096 B | +8,320 B |
| DIRAM code | 74,879 B | 76,867 B | +1,988 B |
| Flash code | 499,340 B | 499,504 B | +164 B |
| Flash data | 232,380 B | 232,460 B | +80 B |
| Loadable image | 849,795 B | 860,347 B | +10,552 B |

DIRAM is shared instruction/data internal SRAM. The 8,320 data bytes comprise the 8,704-byte
sample cache minus 544 removed symbol-cache bytes, plus 160 bytes of state, profiling and
alignment. Total additional internal SRAM is **10,308 B**, with no extra heap allocation or
tables. Subtracting that from the old tightest measured 32,679 B free gives an estimated
22,371 B free with Wi-Fi up. This arithmetic is not an allocation or Wi-Fi measurement.

## Correctness and remaining unknowns

All 26 existing host test scripts and the new geometry test pass. Native `fecbench` produces
all 140 reference CRCs for both plain and fast C. The unchanged encoder's PIE execution still
requires the coordinator's `fecbench` check. QEMU passes 324 existing shaper cases, 2,560
raw/scaled sample-glue cases, 26,114 pack/unpack cases and 864 copy/guard cases. It verifies
instruction semantics, not timing. The stream harness compares every word in two 40-frame
runs of eight modes, tests all allocation failures, and matches all five published dump CRCs.
The ring model tests shifted codes, ten-byte history, starvation and concurrent slot reuse.

LeanDVB decoded the actual host-harness captures for all five baseline configurations:
38, 37, 36, 36 and 36 contiguous payload frames, zero payload bit errors, zero LDPC flips
or failed checks, zero BCH failures. Known-symbol MER was 43.82 dB for QPSK, 43.78 dB at
8 sps and 40.66-40.67 dB at 4 sps. These are host results, not new board measurements.
Reproduce the captures with `python3 tests/test_stream.py notes/fill-dumps`; decode them
using the same rate/callsign arguments as the coordinator recipe below. Detailed logs,
capture files, disassembly and the archived baseline build remain in ignored `notes/`.

Interrupt time is still unknown. IDF v6.1 `esp_wifi/esp32s3/esp_adapter.c:178-191` routes the
MAC source to a supplied CPU number and installs its ISR on the calling CPU. The radio worker
initializes Wi-Fi on core 1 (`radio_s3.c`); the default Wi-Fi task is pinned to core 0. Neither
fact proves the MAC interrupt's final core. Moving tasks or masking interrupts without
measuring their source and duration was rejected. No interrupt saving is in the model.
Actual bus contention, notification spikes, ROM cache-copy costs and code-placement effects
remain unmeasured. The 80 MS/s DAC read traffic is also absent from radio-off tests.

## Coordinator commands and pass criteria

Run from this branch after the coordinator builds and flashes it normally. Every command
below is radio off; no `dvb`, `tx`, `chain` or `wifimem` is requested. Use a new output folder.
Do not change the stored callsign just for this test: the five CRC constants below require
N0CALL, as in the baseline records. With another callsign, compare against a same-callsign
baseline and decode using that callsign instead.

```sh
PY=/Library/Frameworks/Python.framework/Versions/3.13/bin/python3
OUT=notes/max-fill-board
RX=/Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb
mkdir -p "$OUT"
"$PY" host/board.py info --reset --out "$OUT/runs.jsonl"
"$PY" host/board.py membench --timeout 75 --out "$OUT/runs.jsonl"
"$PY" host/shaper.py verify "$OUT/runs.jsonl"
"$PY" host/board.py 'fecbench nibble' --timeout 75 --out "$OUT/fec.jsonl"
"$PY" tests/test_fecbench.py "$OUT/fec.jsonl" --correctness-only
"$PY" host/board.py 'dvbdump 4 40' --timeout 75 --save "$OUT/qpsk.iq" --out "$OUT/runs.jsonl"
"$PY" host/board.py 'dvbdump 13 5m 40 128 1' --timeout 75 --save "$OUT/13-5m.iq" --out "$OUT/runs.jsonl"
"$PY" host/board.py 'dvbdump 15 5m 40 128 1' --timeout 75 --save "$OUT/15-5m.iq" --out "$OUT/runs.jsonl"
"$PY" host/board.py 'dvbdump 13 10m 40 128 1' --timeout 75 --save "$OUT/13-10m.iq" --out "$OUT/runs.jsonl"
"$PY" host/board.py 'dvbdump 15 10m 40 128 1' --timeout 75 --save "$OUT/15-10m.iq" --out "$OUT/runs.jsonl"
"$PY" host/board.py 'iddump N0CALL' --timeout 75 --save "$OUT/id.iq" --out "$OUT/runs.jsonl"
"$PY" host/morse.py "$OUT/id.iq" --rate 10000 --tone 0 --expect N0CALL
"$PY" host/board.py 'dvbpace 13 10m 30' --timeout 60 --out "$OUT/runs.jsonl"
"$PY" host/board.py 'dvbpace 15 10m 30' --timeout 60 --out "$OUT/runs.jsonl"
"$PY" host/board.py 'dvbpace 13 5m 30' --timeout 60 --out "$OUT/runs.jsonl"
"$PY" host/board.py 'dvbpace 15 5m 30' --timeout 60 --out "$OUT/runs.jsonl"
"$PY" host/board.py 'dvbpace 4 30' --timeout 60 --out "$OUT/runs.jsonl"
"$PY" host/board.py 'dvbpace 13 10m 10 128 1000' --timeout 45 --out "$OUT/runs.jsonl"
"$PY" host/stream_profile.py "$OUT/runs.jsonl"
```

Pass correctness before interpreting speed: 30 membench checks; 140 FEC CRCs and 14 zero-mismatch
PIE checks; dump CRCs respectively 1435641601, 1937985440, 1163322913, 474263962, 1757765864;
IDDUMP CRC 1965033598; zero overlap mismatches and matching USB CRCs. Decode all five dumps
with `host/onair.py --stream 40 --callsign N0CALL --mer --shift 0 --receiver "$RX"`, with
`--rate 4000000` for QPSK or `--rate 40000000 --modcod 13|15 --symbol-rate 5000000|10000000`
for 8PSK. Require at least 30 contiguous payload frames, zero LDPC flips/checks, zero BCH
failures and MER above 38 dB. The stall run should insert dummies while retaining valid
sample overlap; underruns are expected only there.

Pass timing on the four unstalled 8PSK runs: `ESP_OK`, zero encoder failures/underruns/late
fills/fatal late, `shaped_words + reused_words == fill_words`, positive slack. The requested
4-sps target is maximum whole-bank fill <=36,000 cycles; the 8-sps no-loss ceiling is
42,936 cycles. Keep QPSK within its old roughly 41k-cycle run spread. Record both startup
and steady-bank maxima if further instrumentation is used, without hiding startup from the
existing maximum. These 40 MS/s commands test the same 4/8-sps fill work; they do not prove
80 MS/s DAC contention. Do not claim 20 Msym/s qualification from this experiment.

The next experiment is to combine this fill with the separate six-symbol pulse after its
merge, then measure the whole path. That removes another lookup per sample without altering
the encoder. Another 3k-6k cycles must disappear for the present pulse to meet the target.
For that merge, `exp/rd-short` already has `shaper_table_t.phase_shift` and `.parts`. Set
`phase_shift = 4` for this packed-ring window and use its two-part scaled dispatch. This
branch's `fill_codes` dispatch supports only the current nine-symbol pulse. The shared
`dvbtx.c`, `shaper.c/.h`, `shaper_pie.S` and sample tests need an explicit integration review.
