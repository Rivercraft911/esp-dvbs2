# Sign windows in the queue

**The page queue uses wild-a sign windows. Silicon admission is pending.**
Round seven retains all 57 main waveform CRCs. Both new 16-frame QPSK dumps
match independent integer convolution; LeanDVB recovers 14 contiguous frames
with zero payload errors, LDPC flips or BCH corrections. The live queue CRCs
are 4118466753 (MODCOD 11) and 1616153472 (MODCOD 4). Full-axis rounding changes
the experimental round-six CRCs, not the retained main waveforms. The standalone
MODCOD 11 kernel dump matches wild-a's pinned CRC 2575466360.

Native and ESP32-S3 QEMU checks cover 2,880 guarded prepares and 17,280 sample
slices. QEMU also passes 140 encoder CRCs, 14 PIE comparisons, 896 packed frames
and 512 quad frames with guarded resident tables. Native lifecycle checks cover
allocation failures, repeated commands, notifications, counter wraps and a
200-frame queue stream. QEMU clocks and native heap sizes are not measurements.
No USB or radio operation was attempted.

## Work and ownership

Core 0 encodes into two guarded quad-code frames and prepares pages between
encoder stages and while waiting for a frame slot. Core 1 renders sign windows.
The wild-b row sums and raw-page handoff are removed. There is no helper task,
per-bank dispatch or join. A page represents 64 groups of five symbols, or
768 DAC samples. Each group holds eleven I-negative bits in word bits 14:4 and
Q-negative bits in 30:20, newest low. Each page owns a readable trailing guard;
its preparation cost shares the same 272 B aligned allocation.

Each four-sample vector uses eight signs. The three chronological windows are
[-6..1], [-5..2] and [-3..4]. Two independent axis tables provide signed 10-bit
DAC fields; rendering is two loads and an OR per vector. For sample p,

`v[p] = round_odd(sum_j sign[j] * L * h[5p-12j] / 2^38)`.

L = 759250125 in Q30 units, h is in 1/256 DAC LSB and is scaled by
`(peak-1)/508`; ties round away from zero. The pulse and derivation are from
exp/moon-fill 73136ce; the sign renderer is from exp/moon-wild-a f704aa0.
`host/machine.py` independently sums the complete axis before rounding.

Preparation accepts the ring's four-symbol bytes directly. Its 4 KiB LUT
separates five new I/Q signs, then scalar shifts preserve the preceding six.
Wild-a's measured 6,210-cycle PIE preparation starts from already packed
five-symbol groups. That number excludes conversion from this ring format and
is not an integrated queue prediction. Every page's measured cost includes its
ring read, packed-group extraction and sign preparation.

The 32-page queue uses single-writer release/acquire publication counters and
separate physical cursors. The producer alone reads/releases frame slots;
queued pages retain no frame pointers. The consumer wakes the producer after
four pages are free. A waiting flag and predicate recheck preserve racing
notifications. Starvation returns an error and fails the gate. The existing
sample cache preserves overlap, fractional phase, PL headers and pilots across
page, frame and physical bank seams.

## RAM

The shaper store uses
`24576 + 2*8352 + 864 + 32*272 + 96 + 4096 + 8144 = 63184 B`:
full-speed sign tables, two frames, dummy, guarded pages, producer window, LUT
and fast encoder workspace. The remaining 2,352 B cannot hold the repeated
7,264 B input fixture, which is an explicit internal allocation during pace.
Its 1,316 B copies and station header are timed; DMA/parser/ISR work is absent.

The QPSK 9/10 encoder's 25,920 B resident allocation holds PRBS, Gold sequence,
paired BCH, LDPC program/columns and GCS header CRC. Header/pilots already live
in the context. Other QPSK rates use their own program and BCH rows. This removes
hot bulk flash-data dependencies; the board must establish the stage miss rate
and full encoder timing. Global cache counters include both cores/interrupts.

Overlap storage is allocated at the active geometry: 8,704 B at 40 MS/s and
3,072 B at 80 MS/s. The Wi-Fi heap probe retains that complete playback layout.
The 8-sps 8PSK dummy borrows the unused PSRAM workspace tail, reclaiming 1,696 B
of internal ring storage; both real frame slots stay internal. These are layout
sizes, not new board heap measurements. All original heap floors are unchanged.

## Admission

At 240 MHz and 80 MS/s the fresh-bank deadline is
`15740 samples / 80e6 samples/s * 240e6 cycles/s = 47220 cycles`.
The 85% ceiling is **40,137 cycles/core/bank**. A QPSK frame lasts
`33282 symbols * 240e6 cycles/s / (100e6/3 symbols/s) = 239630.4 cycles`;
its 85% ceiling is **203,685 cycles/frame**. Frame sizes come from
`host/dvbs2.py`, bank geometry from `radio_s3.h`.

`host/check.sh --moonshot` runs the main dumps/decodes, eight paces and original
three heap floors first. Each main timing ceiling remains 102% of its fixed
measurement. A main failure invalidates admission. The runner keeps going after
failures and exits nonzero. The candidate adds a 16-frame dump/decode, a 30 s
Wi-Fi receive-only pace and a complete-layout heap probe.

Core 1's full-bank maximum includes sign rendering, queue work and cache copies.
Core 0's capacity screen uses the largest sum of actual prepared page costs
charged to any consumed bank. Both edge pages are charged in full, including
again in the next bank if shared. At most 23 pages touch a bank, including the
initial 16,380-sample fill. Add 23 times the maximum measured publication tail,
maximum notification cost, and pure encoder work scaled by 47220/239630.4.
This remains a capacity estimate, not a direct measurement of scheduling tails.
Encoder wall mean/live maximum must also pass independently. Main SPI GCS's
measured 4,823-cycle mean copy remains a planning floor, not a worst-case bound.

Queue low water must be positive, starvation/underruns/lateness zero, slack
positive and Wi-Fi-up/minimum heap at least 20,000 B. Encoder stack headroom
must be at least 512 B. The coordinator must confirm these timing, cache and
heap predictions on the integrated board image.

After building/flashing the committed revision, with N0CALL stored and exclusive
board ownership:

```sh
PYTHON=/Users/river/radioconda/bin/python3 \
RX=/Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb \
  host/check.sh --moonshot notes/moon-round7-board
```

`--plan` opens no device. The runner never flashes, writes NVS or transmits.
