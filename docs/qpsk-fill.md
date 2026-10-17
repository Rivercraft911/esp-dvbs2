# QPSK block fill

The board follow-up found a misaligned encoder workspace in the isolated probe.
It now uses `heap_caps_aligned_alloc(16,...)`. The fill and PIE kernels are unchanged.
The corrected build still needs the radio-off recheck below.

Candidate interface for the encoder engineer: `shaper_qpsk.h`. No `dvbtx.c` changes.

`shaper_qpsk_samples(dst, src, skip, count, table, n)` consumes four QPSK symbols per
byte, oldest in bits 7:6, newest in bits 1:0. Each two-bit code is `phase_k >> 1`,
where phase k is 1, 3, 5, 7. `src[-2..-1]` supplies eight symbols of history.
`skip` is the sample offset (0 through 4*n-1) into the four-symbol block at src[0].
Use n = 3 or 4. Bulk output and table rows are 16-byte aligned; the sample wrapper
handles partial blocks and other output alignments safely. Empty calls touch no buffer.

Blocks start at global symbol indices divisible by four. They must not restart at
frame boundaries: the normal QPSK PLFRAME has 33,282 symbols, or 2 modulo 4.
A window copied from a frame ring must therefore begin at
`4 * floor(first_sample / (4*n)) - 8`, and must include enough complete blocks for
the last requested sample. Pack any missing future symbols in the last block as
phase 1: their taps for the requested earlier samples are zero. Keep the global
block phase through dummy frames and station-ID pauses too.

`shaper_qpsk_build(table,n,level)` takes a caller-owned 16-byte-aligned store and
16..508 DAC LSB peak. Table sizes: 36,864 B at n=3; 49,152 B at n=4. Both reuse
`shaper_store`; its remaining 28,672 B or 16,384 B may hold other immutable data.
No kernel allocation or static data buffer. The pulse is nine symbols, inverse
DAC-hold corrected, roll-off 0.35. New rounding groups require new dump CRCs.

The derivation, resource account and radio-off gates below distinguish instruction
counts, assumptions, host checks and the measurements still needed.

## Result and derivation

The assembled n3 loop models at **22 cycles per 12 samples** (1.833 cycles/sample).
The n4 loop models at **27 cycles per 16 samples** (1.688 cycles/sample).
The estimated worst steady whole banks, including an old 2,348-cycle notification
allowance, are **35,455 cycles at n3** and **33,026 cycles at n4**. Both clear the
40,137-cycle requested ceiling in this model. These remain model estimates. The coordinator subsequently measured isolated
maxima of 34,582/32,248 cycles on e975eb2; see the follow-up below. The isolated
probe does not run the encoder during filling or start Wi-Fi. This agent used
only supplied captures, host tests and QEMU, with no serial access or flashing.

For output symbol j = 0..3 and sample phase p = 0..n-1, table part r = 0..2 uses
four symbols at positions `3-k-4*r`, k = 0..3, relative to this block's start:

```
T[r,w,j*n+p] = round(sum_k L[2*((w >> (2*k)) & 3)+1]
                         * h[(j-(3-k-4*r))*n+p] / 2^38)
y[j*n+p] = T[0,new,j*n+p] + T[1,previous,j*n+p] + T[2,oldest,j*n+p]
```

Here h is zero outside the 9*n-tap pulse and has units 1/256 DAC LSB. L is an
integer QPSK coordinate in units 2^-30. The integer result is in DAC LSB.
`host/shaper.py:corrected_rrc` derives the inverse-hold root-raised-cosine pulse;
`host/qpsk_fill.py --header` generates the two tap arrays. They are scaled to
level-2 LSB before three independent roundings, whose error is at most 1.5 LSB
per axis. For each sample, nonnegative per-part biases sum to 512. Tests exhaust
all rows' independent extrema: each field is nonnegative and the sum stays
within 0..1023. Consequently packed 32-bit addition cannot carry from I into Q.
XOR 0x80200 converts the two offset-binary fields into signed ten-bit samples.

A four-symbol byte has 4^4 = 256 values. Three parts each have 256 rows of 4*n
words, so the table occupies `3 * 256 * 4*n * 4 = 12,288*n B`. At n3 the rows
are exactly three vectors, without padding. Across blocks, a new window becomes
part 1, then part 2. Updating two existing pointers and reading one new byte
avoids reconstructing three overlapping symbol indices per output block.
The n3 body contains six address/input instructions, fifteen vector instructions
and one input-pointer increment. All six additions share a load or store.

Instruction semantics and the issue/interlock equation are from the
[ESP32-S3 TRM v1.8](https://www.espressif.com/sites/default/files/documentation/esp32-s3_technical_reference_manual_en.pdf),
1.7.1 pp. 65-66, Table 1.7-2 pp. 68-69, fused add/load and add/store in 1.8.74-75
pp. 150-151, aligned vector loads in 1.8.88 p. 164, stores in 1.8.192 p. 275.
`host/shaper_cycles.py` replays the actual disassembly. Four prologue interlock
cycles remain; the steady loop has none under that model. QEMU checks semantics,
never timing. This extends the same issue model used by the previous block4
experiment; its reported silicon agreement does not validate this new complete fill.

## Bank estimate

The probe uses `radio_s3.h`'s current 80 MS/s geometry, including both physical
pieces of a wrapping bank. Its private 768-word circular sample cache is indexed
by the global sample position, so source and destination vector alignment match.
It saves only newly shaped samples. No read is made from an active DAC bank.

```
fresh samples/bank = 16,380 - 128 - 512 = 15,740
period = 15,740 / 80e6 s = 196.75 us
budget = 196.75 us * 240 cycles/us = 47,220 cycles
15% margin ceiling = 0.85 * 47,220 = 40,137 cycles
startup cache = (640 overlap + 128 extra lead) * 4 = 3,072 B
```

The model exhausts 819 bank/block phases at n3 and 3,276 at n4. It includes two
cache saves where necessary, short whole-block calls and partial-block copies.
Only the kernel term is an assembled instruction count. The other terms below
are explicit allowances, not measured maxima.

| Estimated worst steady bank | n3 cycles | n4 cycles |
|---|---:|---:|
| Kernel, including partial blocks and prologues | 28,994 | 26,772 |
| Packed input copies, assumed 1 cycle/B | 1,317 | 990 |
| Partial output copies, assumed 6 cycles/word | 48 | 168 |
| Sample-cache copies, 0.5 cycles/word plus 160 setup cycles | 1,248 | 1,248 |
| Division, callback, bookkeeping and scheduler allowance | 1,500 | 1,500 |
| Isolated probe total | **33,107** | **30,678** |
| Old stream notification maximum | 2,348 | 2,348 |
| Candidate integrated total | **35,455** | **33,026** |

Startup shapes all 16,380 samples: estimated 33,468 cycles at n3 and 30,840 at n4.
The full n3 kernel alone is 30,057 cycles; the n4 benchmark emits 16,368 words in
27,646 modeled cycles, with the sample wrapper accounting for the bank's last 12.
The 2,348-cycle notification is a previous measured maximum from
[buildout](../results/2026-10-16-buildout/README.md), not a bound and not work
performed by this pre-encoded probe. The model allows only packed-byte copying;
converting a nibble/raw ring into the new format would be additional work.

A 25% increase of the probe subtotal, then notification, gives 43,732 cycles at
n3 and 40,696 at n4. Both miss the 15% target. This is a sensitivity check, not
a statistical bound. SRAM contention with the encoder and DAC, interrupts,
ring-boundary stitching and the actual callback overhead require board measurement.
The prior 45.1k-cycle keyed n4 8PSK fill is measured; comparing it to 33.0k here
is a hypothesis about a different alphabet, not an observed improvement.

## Memory

The existing 65,536 B static table store and both DAC banks are reused. No new
static data buffer is linked. Against 3c4f815, same IDF v6.1 default build:

| ELF section | Base B | Candidate B | Change B |
|---|---:|---:|---:|
| IRAM text | 92,311 | 93,499 | +1,188 |
| IRAM end alignment | 101 | 193 | +92 |
| DRAM data | 35,096 | 35,096 | 0 |
| DRAM BSS | 80,592 | 80,592 | 0 |
| Flash text | 500,764 | 503,996 | +3,232 |
| Flash rodata | 240,812 | 241,900 | +1,088 |

Effective additional permanent internal capacity: **1,280 B**, including probe
and the PIE/sample wrapper. The reference C kernel is in flash; its membench
timing is not an IRAM-kernel comparison. This reduces the old tightest 8PSK 21,211 B free figure to
an estimated 19,931 B; Wi-Fi heap has not been remeasured. It is below that run's
20 KB floor. This experiment is not automatic adoption into all transmit modes.

During `membench`, input borrows bank 1 and output bank 0: no extra heap.
During an isolated paced probe, requested internal heap is **21,121 B**:
4,480 B state (3,072 B cache, 1,368 B packed window, 40 B fields/alignment) plus
16,641 B for two pre-encoded frames. Allocator overhead is additional; records
include actual `state_heap_bytes`, `source_heap_bytes`, free and largest internal
block. Preparation also temporarily allocates the existing fast encoder context
internally and workspace/payload/raw frame in PSRAM, then frees them before timing.
For a 16-frame dump the 133,128 B packed source and 4,104 B USB state are in PSRAM;
its timing is not comparable to the internal-input pace. The assembled builder uses a 640 B stack frame plus a 48 B value-helper frame;
the sample wrapper uses 112 B and its PIE callee 32 B. The probe fill uses 64 B.
These live within existing command/worker stacks, not new persistent allocations.

An integration budget, **conditional on a two-bit frame ring**, with 16-byte
alignment and 16 guard bytes per slot, for QPSK 9/10:

| Storage | B |
|---|---:|
| Three packed normal frames: 3 * 8,352 | 25,056 |
| Packed dummy, 864 B | 864 |
| Payload: (58,192-80)/8 | 7,264 |
| Ring/dummy/payload total | 33,184 |
| Existing FEC workspace moved from PSRAM | 12,420 |
| Additional resident BCH table at 9/10 (t=8) | 0 |

At n3 the unused store supplies 28,672 B, leaving a 4,512 B ring heap request.
At n4 it supplies 16,384 B, leaving 16,800 B. Reusing dvbtx's existing window and
sample cache, and retaining the same MODCOD context, net additional internal
requests including code are **18,212 B / 30,500 B**. Against the brief's roughly
53 KB free QPSK baseline, that suggests roughly 35 KB / 23 KB remaining, not a
Wi-Fi allocation result. Rates needing an 8,192 B resident BCH table must add it.
A nibble-packed ring is twice as large and cannot use this arithmetic. The encoder
engineer owns that choice and its conversion/stitching cost. This branch does not
allocate a live ring, move the workspace, or change any encoder.

## Correctness and quality

All 30 host test scripts pass, including all 140 FEC CRCs and the five unchanged
dump CRCs. Both firmware and the QEMU shaper app build cleanly under IDF v6.1.
The native probe runs the real command, pre-encoder and fill callback. Every dump
sample matches the independent Python encoder and table model. It checks source
wraps and frames ending at a half-byte, 822 bank schedules per mode, allocation
failure cleanup, invalid arguments, empty callbacks and invalid backward requests.
The kernel harness checks 19,368 guarded slices at every sample/output alignment,
three seeds and levels 16, 128 and 508, on native C and QEMU. All pass. Existing
QEMU shaper, pack/unpack, copy and sample-glue cases also pass.

Sixteen-frame host dumps, N0CALL, peak 128 LSB, no decimation:

| Mode | IQ CRC32 | LeanDVB payload frames | Raw-sample MER |
|---|---:|---:|---:|
| n3, MODCOD 4 | 3388792897 | 14 | 37.99 dB |
| n3, MODCOD 11 | 1271511239 | 14 | 37.98 dB |
| n4, MODCOD 4 | 3789273049 | 14 | 41.22 dB |
| n4, MODCOD 11 | 2081653259 | 14 | 41.21 dB |

All recover contiguous bit-exact payload with zero LDPC flips/checks, BCH failures
or corrected bits. The sample pulse deliberately compensates a DAC hold absent
from these dumps. An 8,192-symbol host loop with an ideal hold measures 45.02 dB
at n3 and 44.64 dB at n4. Linear-tap upper-mask margins are +0.154 and +0.155 dB,
using `host/shaper.py`'s EN 302 307-1 Annex A template; lower margin is +0.250 dB.
These are digital models. Neither decoding nor linear taps qualify the quantized
signal's complete spectrum or the actual radio/filter.

At n3, `Rs = 80e6/3 = 26.666666... Msym/s`, occupied width `(1+.35)*Rs = 36 MHz`,
and 9/10 payload `(58,192-80)*Rs/33,282 = 46.561 Mb/s`. The one-core frame budget
is 299,538 cycles; 15% reserve requires the independent encoder below 254,607 cycles.
No encoder speed claim is made here.

Rejected: a whole nine-symbol complex table needs 4^9 entries and 3,145,728 B even
without vector padding at n3. A shortened pulse sacrifices response margin when
the nine-symbol table already fits. Separate I/Q tables shrink individual tables
but need extra combines and input-bit extraction. The block layout removes all
three-sample vector packing while leaving the pulse length intact.

## Reproduce, no devices

```sh
PY=/Library/Frameworks/Python.framework/Versions/3.13/bin/python3
"$PY" tests/test_qpsk_fill.py
"$PY" tests/test_qpsk_probe.py notes/qpsk-dumps
. ~/.espressif/tools/activate_idf_v6.1.sh >/dev/null 2>&1
IDF_COMPONENT_MANAGER=0 idf.py -C firmware build
xtensa-esp32s3-elf-objdump -d --no-show-raw-insn \
  firmware/build/esp-idf/main/libmain.a > notes/qpsk.dis
"$PY" host/qpsk_fill.py --disassembly notes/qpsk.dis
```

Run the [shaper QEMU procedure](../tests/shaper_pie/README.md), then pass its log to
`tests/test_qpsk_fill.py`, `tests/test_shaper.py` and `tests/test_tx_samples.py`.
`IDF_COMPONENT_MANAGER=0` avoids this sandbox's denied process-discovery call;
there are no managed components. It does not change the target configuration.

## Board follow-up, 2026-10-17

The coordinator's e975eb2 captures first differ at sample 270 (n3) or 360 (n4),
symbol 90, bank 0, four-symbol block 22, packed source byte 22 / window byte 24.
Part 0 selects row `0x11` instead of `0x1f`; parts 1 and 2 match. This is the
first data symbol after the 90-symbol PLHEADER, before any bank wrap.

The pre-encoder allocated `dvbs2_workspace` with ordinary malloc, violating the
16-byte alignment contract in `dvbs2.h`. Captures imply offset 12 modulo 16:
PIE BB stores round down by 12 bytes, leaving a stale 12-byte gap near the BB tail;
QPSK mapping reads from 12 bytes before the workspace. BCH and LDPC then encode
corrupted input. The gap's old contents explain why n3/n4 parity differs.

Every sample can be inverted through the table to recover the packed input.
QEMU at offset 12, seeded with only the recovered 12-byte prefix and 12-byte
untouched gap, reproduces all four board dumps exactly, including their failing
CRCs. Aligned QEMU output matches Python. No phase scaling, table placement,
packing, bank/window or LDXQ compensation change is needed. The old native test
always aligned malloc to 16 bytes; the regression now deliberately returns
8-modulo-16 ordinary allocations and checks the encoder's alignment contract.
It fails before the fix and passes after it.

The corrected production preparation/window/PIE fill in QEMU matches all
7,455,168 host samples. LeanDVB recovers 14 payload frames per dump with zero
payload errors, LDPC flips/checks and BCH failures/corrections. MER remains
37.98..37.99 dB (n3), 41.21..41.22 dB (n4). IDF v6.1 builds successfully.

Coordinator records are in `notes/qpsk-fill-board/`; the full sample comparison,
QEMU reproduction and stale-byte derivation are in `notes/qpsk-fill-fix.md`.
These are supplied board measurements, not measurements of the corrected build:

| e975eb2, radio off | n3 | n4 |
|---|---:|---:|
| PIE membench minimum, cycles | 30,069 | 27,659 |
| 30 s pace maximum fill, cycles | 34,582 | 32,248 |
| Minimum slack, cycles | 12,369 | 14,701 |
| Late fills | 0 | 0 |

Both paces report `ESP_OK`, internal input and `encoder_timed:false`. These maxima
are below the 35,455/33,026-cycle estimates **including** a notification allowance.
They exceed the fill-only 33,107/30,678-cycle estimates by 4.5%/5.1%. They do not
measure encoder integration. The four membench hashes, 140 FEC CRCs, 14 PIE checks
and the coordinator's four unchanged dumps passed. The corrected allocation runs
only before filling; the timed fill and both kernel instruction streams are unchanged.

## Coordinator: radio-off recheck

Record the corrected firmware revision in a fresh folder. Keep the radio off.
For N0CALL and 16 frames, the expected CRCs remain:

| Command | IQ CRC32 |
|---|---:|
| `dvbdump qpsk3 16 4` | 3388792897 |
| `dvbdump qpsk3 16 11` | 1271511239 |
| `dvbdump qpsk4 16 4` | 3789273049 |
| `dvbdump qpsk4 16 11` | 2081653259 |

1. `membench`: require all existing checks and the four QPSK checks to match.
   QPSK PIE minima should remain near 30,069/27,659 cycles.
2. Four dumps above: require expected CRC, USB CRC agreement, zero overlap
   mismatches and `ESP_OK`. Decode each with LeanDVB: expect 14 contiguous payload
   frames, zero payload errors, LDPC flips/checks and BCH failures/corrections;
   MER >=37 dB (n3), >=40 dB (n4).
3. `dvbpace qpsk3 30 11` and `dvbpace qpsk4 30 11`: compare against the measured
   maxima/slack above. Require zero late/fatal fills, `ESP_OK`, internal source,
   `encoder_timed:false`; retain ceilings 40,137/36,000 cycles and slack >=7,083.

```sh
PY=/Library/Frameworks/Python.framework/Versions/3.13/bin/python3
OUT=notes/qpsk-fill-recheck
RX=/Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb
mkdir "$OUT"
"$PY" host/board.py info --out "$OUT/runs.jsonl"
"$PY" host/board.py membench --timeout 75 --out "$OUT/mem.jsonl"
"$PY" host/shaper.py verify "$OUT/mem.jsonl"
"$PY" host/qpsk_fill.py --membench "$OUT/mem.jsonl"
for n in 3 4; do
    for mc in 4 11; do
        "$PY" host/board.py "dvbdump qpsk$n 16 $mc" --timeout 75 \
            --save "$OUT/qpsk$n-$mc.iq" --out "$OUT/dumps.jsonl"
        if [ "$n" = 3 ]; then RS=26666666.666666668; else RS=20000000; fi
        "$PY" host/onair.py "$OUT/qpsk$n-$mc.iq" --rate 80000000 --scale 512 \
            --symbol-rate "$RS" --modcod "$mc" --stream 16 --callsign N0CALL \
            --mer --shift 0 --receiver "$RX" > "$OUT/dec-$n-$mc.json"
    done
    "$PY" host/board.py "dvbpace qpsk$n 30 11" --timeout 60 --out "$OUT/pace.jsonl"
done
```

## Changed files

Kernel and interface: `firmware/main/shaper_qpsk.{c,h}`,
`firmware/main/shaper_qpsk_pie.S`, `firmware/main/shaper_qpsk_taps.h`.
Probe and registration: `firmware/main/qpsk_probe.{c,h}`, `firmware/main/main.c`,
`firmware/main/membench.c`, `firmware/main/CMakeLists.txt`.
Models: `host/qpsk_fill.py`, `host/shaper_cycles.py` (adds ADDX2 modeling).
Checks: `tests/qpsk_fill.c`, `tests/qpsk_probe_host.c`, `tests/test_qpsk_fill.py`,
`tests/test_qpsk_probe.py`, `tests/shaper_pie.c`,
`tests/shaper_pie/main/CMakeLists.txt`, `tests/shaper_pie/README.md`,
`.github/workflows/ci.yml`. This document is `docs/qpsk-fill.md`.
The production encoder and `dvbtx.c` are unchanged by this front.
