# Queue transmitter

**QPSK 9/10 at 33.333333 Msym/s is wired, but not admitted on silicon.**
Round six builds and retains all 57 waveform CRCs. Its native 16-frame queue
stream retains CRC32 1710089068. LeanDVB recovers 14 contiguous frames with
zero LDPC flips, BCH corrections or payload errors. No USB or RF operation
was attempted. The coordinator must measure the integrated Wi-Fi-up design.

## Ownership and work

Core 0 encodes into two guarded quad-code frame slots, then prepares a
persistent queue between encoder stages and while waiting for a slot. Core 1
renders the banks. There is no helper task, per-bank dispatch, join, encoder
callback, or live split-kernel fallback. The unsuccessful experimental 8PSK
rates and old `membench moon` probe are removed from the live image.

A page represents 64 groups of five QPSK symbols, or 768 DAC samples. Two pages
out of three contain three 16-bit table offsets per group. Every third page
contains the packed symbols and six history symbols; core 1 prepares that page
before rendering it. Raw input is copied before expanding offsets in place.
This moves one third of preparation into core 1's slack. All source windows
start at a four-symbol boundary, avoiding the scalar prepare prefix.

The queue has 48 pages. Single-writer 32-bit publication and consumption
counters use release/acquire ordering. Separate physical page cursors preserve
the 48-page wrap when the counters wrap; 64-bit symbol/sample cursors preserve
the waveform timeline. Native tests cross the counter wrap on every start.
The producer sleeps only when both its frame ring and queue are full. The
consumer wakes it after four pages have become free. A waiting flag and a
predicate recheck preserve notifications racing the wait. Queue starvation
returns an error immediately and fails admission.

The encoder alone reads/releases frame slots. Each queued page owns its
history and has no pointers into released slots. The renderer retains the
existing DAC overlap cache. PL headers, pilots, station headers and fractional
phase continue across frame and bank boundaries. Station ID pauses the payload
timeline through the existing radio wrapper.

Store use is `27648 + 2*8352 + 864 + 48*384 + 96 = 63744 B`: tables,
frames, dummy, queue and producer window, respectively. The 65,536 B shaper
store leaves 1,792 B. The pace's repeated 7,264 B source is an explicit internal
allocation; its 1,316 B copy chunks and station header are timed. This fixture
does not measure DMA, parsing, CRC checks or input interrupts.

## Budget and evidence

At 240 MHz and 80 MS/s, the fresh-bank period is
`15740 samples / 80e6 samples/s * 240e6 cycles/s = 47220 cycles`.
The 85% ceiling is **40,137 cycles/core/bank**. The frame period is
`33282 symbols * 240e6 cycles/s / (100e6/3 symbols/s) = 239630.4 cycles`;
its 85% ceiling is **203,685 cycles/frame**. Frame dimensions come from
`host/dvbs2.py`; bank geometry comes from `radio_s3.h`.

Coordinator records in `notes/wild-b-board/runs.jsonl` give prepare maximum
20,891 and render maximum 31,761 cycles for 1,364 groups, radio off. Round-five
`notes/moon-round5-board/runs.jsonl` gives 146,860 cycles/frame and about 53,425
payload cycles at QPSK 26m667. Main's immutable SPI GCS replay gives
`ceil(57972998 / 12021) = 4823 cycles/frame` of copy, a measured mean, not a
worst-case bound. Replacing synthetic generation predicts
`E = 146860 - 53425 + 4823 = 98258 cycles/frame`.

With `G = (15740/12)/1364`, kernel-only estimates are
`C0 = (2/3)*20891*G + E*47220/239630.4 = 32755 cycles/bank`, and
`C1 = (31761 + 20891/3)*G = 37239 cycles/bank` (rounded).
These combine measurements from different contexts. Queue wrappers, cache
copies, notifications, interrupts and Wi-Fi are additional and unbounded here.
A slow 200,000-cycle encoder fails the same model. These are planning screens,
not claims of deadline compliance.

Wild-a's coordinator results arrived in `notes/wild-a-board/membench.log`:
prepared-history render median/max 35,527/36,947 cycles for 16,380 samples;
history prepare 6,210/8,678. Wild-b render median/max is 30,041/31,761 for
16,368 samples. Sign windows offer cheaper preparation but a slower renderer.
Use wild-b's renderer and move preparation into its spare capacity. Only this
queue implementation is linked; no sign-window code is imported.

## Main regressions

Legacy QPSK 5m/10m returns to register-nibble BCH. Its paired flash table
introduced repeated cache misses in round five; the faster paired BCH and quad
mapper remain in 80 MS/s QPSK. Packed payloads now borrow encoder scratch until
BB consumes them, removing a redundant 7,264 B heap payload in 8PSK 9/10 5m.
Byte BCH borrows available store tails in 8PSK 20m and MODCOD 13 10m. QPSK
26m667 uses two slots and an 8,144 B fast workspace plus window in the store,
removing its 12,420 B heap workspace. Plain stages still require the full type.

Fecbench borrows the idle shaper store for workspace, packed output and BCH,
avoiding the allocation failure. Ingress CRC tables are allocated only during
input operation, preserving the slicing-by-four input path while reclaiming
4,096 B between sessions. The encoder stack is 2,048 B instead of 3,072 B;
round-five measurements left at least 2,108 B free. Every pace now requires at
least 512 B stack headroom. Heap and timing improvements remain unmeasured. The local QEMU executable
did not reach serial boot output; no new QEMU execution result is claimed.
The firmware and both correctness apps build with ESP-IDF v6.1.

## Coordinator gate

`host/check.sh --moonshot` first runs all retained main dumps/decodes, eight
main paces and the three original heap floors. The four critical paces last
30 s. Fill maximum, encoder mean and live maximum must each be at most 102%
of fixed main measurements. No heap floor or timing ceiling is relaxed.

The candidate adds its 16-frame dump/decode, 30 s Wi-Fi receive-only pace and
Wi-Fi heap probe. Full-bank core-1 maximum includes local preparation, render,
queue work and overlap copies. Core-0 capacity uses at most 15 prepared and 8
raw page maxima per bank, including ring reads and publication, plus maximum
measured bank wake cost and pure encoder work scaled to the bank period. Pure
encoder work excludes synchronous queue preparation; the measured SPI copy
floor is retained. Encoder wall mean/live maximum also pass independently.
Queue low water must be positive, starvation/underruns/lateness zero, slack
positive and Wi-Fi-up/minimum heap at least 20,000 B. A failed main gate
invalidates QPSK admission. The runner continues after failures and exits nonzero.

After building/flashing the committed revision, with N0CALL stored and exclusive
board ownership:

```sh
PYTHON=/Users/river/radioconda/bin/python3 \
RX=/Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb \
  host/check.sh --moonshot notes/moon-round6-board
```

`--plan` opens no device. The runner never flashes, writes NVS or transmits.
