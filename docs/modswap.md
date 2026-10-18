# Five-level VCM shaping experiment

`exp/modswap`, based on `d9520e8`. Opt in with `vcm axes on`, then select a
normal VCM pattern. This prototype accepts **5 Msym/s only**. The default
full-table path remains available with `vcm axes off`; `vcm off` disables VCM.
The agent did not access the board or radio. The coordinator subsequently
qualified the 5 M pattern with radio off; see the [board decode and merge assessment](modswap-board.md).

## Estimates and selection

### First seed: QPSK as an 8PSK subset

[EN 302 307-1 V1.4.1, figures 9 and 10, sections 5.4 and 5.5](https://www.etsi.org/deliver/etsi_en/302300_302399/30230701/01.04.01_60/en_30230701v010401p.pdf)
gives this mapping before physical scrambling:

| QPSK label | Phase index k, angle k*pi/4 | 8PSK label at that point |
|---|---:|---|
| 00 | 1 | 000 |
| 01 | 7 | 101 |
| 10 | 3 | 110 |
| 11 | 5 | 011 |

These labels select a waveform; **do not regroup the QPSK coded bits into
triples**. The encoder already emits canonical phase indices. PLHEADER uses
the odd phases; pilots start at pi/4 and physical scrambling adds multiples
of pi/2, retaining that subset. Unit symbol energy and pulse scaling agree.

At 5/10 Msym/s, both legacy tables use identical integer taps and axis levels.
They round each three-tap part once, with identical shift and tie rule. Their
per-part biases can differ, but always sum to 512 per axis/sample. Thus the
final samples agree **exactly**, given the same representable symbol history.
The mixed renderer also retains even-phase history across an 8PSK-to-QPSK
boundary. Startup phase-zero history differs from QPSK-only initialization;
that is a startup transient, not a rounding discrepancy. Exhaustive tests
explicitly use odd-phase history when comparing with QPSK's own table.

Tiny quantization differences could still decode at adequate SNR, but there
is no reason to accept them here. All comparisons require exact DAC words.

The prototype already shares one full eight-phase table at 5/10 Msym/s.
Adding subset mapping therefore saves **zero additional bytes** over d9520e8.
Table-only costs if starting from separate modulation tables would be:

| Rs | QPSK + 8PSK tables | One full eight-phase table |
|---|---:|---:|
| 5 Msym/s | 8,192 + 65,536 B | 65,536 B |
| 10 Msym/s | 4,096 + 32,768 B | 32,768 B |
| 20 Msym/s | 49,152 + 16,384 B, different native pulses | 32,768 B for a new common nine-symbol pulse |

At 240 MHz, the assembly instruction estimates below exclude setup, tails,
symbol unpacking, cache copies, notification, interrupts and SRAM contention.
They apply equally to QPSK and 8PSK through the full eight-phase renderer.
The bank column counts freshly shaped samples, not the reread overlap:

| Rs / DAC rate | Cycles/symbol, issue estimate | Render cycles/fresh bank | Full bank deadline |
|---|---:|---:|---:|
| 5 / 40 M | 14 + 1/6 | 25,266 | 85,608 |
| 10 / 40 M | 9 + 1/6 | 32,698 | 85,608 |
| 20 / 80 M, hypothetical common pulse | 9 + 1/6 | 36,071 | 47,220 |

These are estimates from `shaper_pie.S`, not measured bank-fill times.
The native 20 M QPSK and 8PSK paths use different pulse lengths, table rounding,
queue formats and history. The 20 M row does not establish mixed-mode support.

### Second seed: copy on a modulation switch

Using the supplied **59 MB/s** PSRAM read measurement as an optimistic copy
ceiling, `copy_us = bytes/59` and `dummy_us = 3330/Rs_M`:

| Rs | QPSK table / copy | 8PSK table / copy | Dummy duration | Minimum dummies for Q / 8 |
|---|---|---|---:|---:|
| 5 M | 8,192 B / 138.8 us | 65,536 B / 1,110.8 us | 666 us | 1 / 2 |
| 10 M | 4,096 B / 69.4 us | 32,768 B / 555.4 us | 333 us | 1 / 2 |
| 20 M, common nine-symbol pulse | 4,096 B / 69.4 us | 32,768 B / 555.4 us | 166.5 us | 1 / 4 |
| 20 M, existing separate pulses | 49,152 B / 833.1 us | 16,384 B / 277.7 us | 166.5 us | 6 / 2 |

Actual copies also write internal RAM and contend with playback. These are
lower bounds, with no guard. Copying/rebuilding boundary history adds work.
Dummy airtime alone does not make the implementation safe: its renderer must
remain independent of the slot being overwritten. That requires another
small QPSK table, a separate dummy renderer, or sufficient prepared samples.
At 5 M, one dummy occupies 106,560 sample bytes; two exceed both DAC banks
together. Reading prepared samples at 160 MB/s from 59 MB/s PSRAM is infeasible.
The 64 KiB internal slot also remains reserved. Copying would add orchestration
and airtime without fixing the existing full-table prototype's memory layout.

For a fixed two-dummy policy, overhead at an observed switch frequency `f`
is `f * 6660 / Rs`. At 5 M, 1/10/100 switches per second consume
0.1332/1.332/13.32% airtime. Alternating QPSK and 8PSK after every N data
frames costs `13320/(55476*N+13320)`: 19.36% at N=1, 5.66% at N=4,
1.48% at N=16. An ideal direction-specific 1/2-dummy policy costs 15.26%
at N=1. Standard dummy PLFRAMEs carry no payload and have no pilots.

### Selected: factor by axis, retain exact three-tap rounding

Each axis takes only `{0, +/-1/sqrt(2), +/-1}`. Three-symbol windows therefore
have **125** distinct scalar values rather than 512 complex values. One set
serves both axes and both modulations. Splitting into individually rounded
single-tap contributions would change samples; this design retains the
original three-tap sum and rounding.

At eight samples/symbol, scalar rows use eight uint16 lanes. Physical part
order `[2,0,1]`, padded to 128 rows/part, permits one `addmi` for adjacent
parts. The table is 6,144 B. A 4,096 B map translates each complex window
into two direct row addresses. Total: **10,240 B**, versus 65,536 B.
On 64-bit hosts the map holds 32-bit offsets instead of pointers; the scalar
decoder adds the table base. The target C and PIE paths use actual addresses.

An earlier 8 KiB variant packed two offsets per map entry. Direct addresses
remove seven instructions per symbol. Reusing only the documented fast
workspace prefix offsets the larger map, and gives this final layout:

| Existing 65,536 B store, byte offsets | Bytes |
|---|---:|
| 0: table + direct address map | 10,240 |
| 10,240: two maximum QPSK-sized packed slots | 33,344 |
| 43,584: packed dummy, including unpack guard | 1,696 |
| 45,280: fast workspace prefix | 8,144 |
| 53,424: render window + alignment | 2,064 |
| 55,488: 2,176-word overlap cache | 8,704 |
| 64,192: spare | 1,344 |

Internal **allocation requests fall from 44,106 B to zero** at 5 M, excluding
task stacks, ingress/Wi-Fi allocations, allocator overhead and existing
statics/DAC banks in both cases. The maximum encoder context remains in
PSRAM: 17,696 B on S3, 17,744 B under the 64-bit host ABI. The old 12,420 B
PSRAM workspace is eliminated. Fast stages alone may use this layout; the
plain encoder's parity workspace would overlap the render window/cache.

The actual assembled steady loop has 193 instructions per six symbols:
**32 + 1/6 per symbol**, about **57,369 cycles per fresh bank** at 5 M.
Phase loads are separated from their uses. Allowing roughly 10–15k cycles
for existing non-render work suggested about 67–72k cycles/bank before board
testing. The coordinator subsequently measured 70,730 cycles maximum in a
Wi-Fi-up pace. The old 8 KiB offset-map version needed about
72k renderer cycles alone. PIE packing adds each axis independently, shifts
I by six, interleaves uint16 lanes, shifts each word back by six, then XORs
the existing DAC bias. Lane bounds prevent carries and saturation.
The [S3 TRM, chapter 1](https://documentation.espressif.com/esp32-s3_technical_reference_manual_en.pdf)
defines the load, fused add, zip and shift instructions used here.

No switch copies, additional queue pages or mandatory dummy airtime are
introduced. The existing underrun dummy policy still applies. A maximum
QPSK frame takes 6.6564 ms, an 8PSK frame 4.4388 ms at 5 M; encoding including
mode preparation must fit those periods on average. The shortest frame
budget is 1,065,312 CPU cycles. Profile the boundary as well as the FEC kernel.

## Rate limits

* **1 M:** original QPSK VCM only; a full 8PSK table would need 262,144 B.
  An axis implementation could fit and has ample CPU budget, but n40 kernels
  and validation are outside this prototype.
* **5 M:** full-table VCM exists; the compact path removes its playback heap
  requests and passed coordinator radio-off timing/heap checks for 4/11/13/17.
* **10 M:** original common-table VCM is host-verified but heap-heavy
  (59,744 B additional internal requests on S3). The compact n8 renderer
  cannot be sped up simply by running it twice as fast. A four-sample variant
  saves three vector instructions, still roughly 29 + 1/6 cycles/symbol
  against a 24-cycle budget before other work. It needs a different pipeline.
* **20 M:** a common legacy pulse has an attractive renderer-only lower bound,
  but just 12 cycles/symbol including all fill work; native optimized pulses
  and queues are incompatible across a modulation boundary. Unimplemented.
* **26.67/33.33 M:** native pipelines differ; 33.33 M has no matching 8PSK path.
  Neither is supported for mixed modulation here.

## Verification and coordinator handoff

Host CI tests pass and fixed-mode golden CRC files are unchanged. Tests check
all 512 complex windows in all three pulse parts, levels 16/128/508, QPSK's
own table, all six rotations, lengths 0..257, sample cuts and write guards.
The **actual optimized PIE assembly** passes these tests in S3 QEMU; QEMU
provides functional evidence, not board timing.

The native firmware lifecycle uses a synthetic 30,000 B internal heap with
no available block over 2,048 B. Compact setup requests zero internal bytes;
the original allocator fails cleanly under the same constraints. Repeated
starts, PSRAM allocation failure, Wi-Fi probe and Wi-Fi-paced routing pass.
Every output word is checked against a separately built full-table oracle.

LeanDVB at `/Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb`
decodes interleaved QPSK/8PSK across all 14 MODCODs. Single-frame dwell recovers
27 frames/163,240 contiguous bytes; two-frame dwell recovers 55/333,744.
The requested 4/11/13/17 pattern at three-frame dwell recovers 22/128,956;
in-band 4→13 control recovers 45/240,296. All report zero LDPC flips,
BCH corrections/failures and rejected BBHEADERs. Acquiring/trailing frames
are excluded by the receiver. These are host IQ captures, not RF evidence.

```sh
python3 tests/test_shaper_axes.py
RX=/Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb
python3 tests/test_vcm.py --axes --receiver "$RX" --output notes/modswap/axes
(. ~/.espressif/tools/activate_idf_v6.1.sh && cd firmware && IDF_COMPONENT_MANAGER=0 idf.py build)
```

The standalone `tests/shaper_axes` IDF project runs the same guarded tests
through PIE. Build it with `SDKCONFIG` and the build directory under `notes/`;
merge its bootloader, partition table and app into a flash image, then run
`qemu-system-xtensa -nographic -machine esp32s3 -drive file=flash.bin,if=mtd,format=raw`.
Stop after `QEMU-END`. No hardware is involved.

Only the coordinator runs the following board console commands, radio off,
after flashing this branch with the existing station configuration:

```text
spiinput off
vcm axes on
vcm 1 4 11 13 17
wifimem 4 5m
dvbdump 4 5m 64
dvbpace 4 5m 30
vcm off
vcm axes off
```

Use the normal binary-aware dump capture tool for `dvbdump`. `dvbpace` now
uses Wi-Fi NULL/RX for VCM, keeping TX unkeyed. Memory setup reports free and
largest internal blocks before allocations; allocation errors additionally
report free/largest PSRAM. Final records include table bytes, cache store
bytes, ring heap bytes, allocator residency, free/largest/minimum internal
heap, boundary cycles, mode counts and `fill_kernel: pie-axes5`.
Require minimum internal heap >=20,000 B, zero overlap mismatches, encoder
failure/late/fatal counters all zero, positive fill slack, and >=256 B encoder
stack reserve. Record the measured reserve on both cores before considering
any separate RF experiment.
