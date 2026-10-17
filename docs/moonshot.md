# Direct sign mapping

Round eight maps QPSK directly into the sign queue. Silicon admission is pending.
All 57 retained host waveform CRCs pass. The new 16-frame CRCs remain
4118466753 (MODCOD 11) and 1616153472 (MODCOD 4); LeanDVB recovers 14 contiguous
frames from each with zero payload errors, LDPC flips or BCH corrections.
QEMU checks 96 direct-mapper frames, 140 encoder CRCs, 896 packed frames,
512 quad frames, 2,880 guarded prepares and 17,280 render slices. These establish
correctness, not board timing or heap headroom.

## Ownership

Core 0 maps FEC bytes into its existing scratch, then emits eleven-sign histories
straight into queue pages. It inserts the PL header and pilots while mapping.
There is no packed PLFRAME, ring-window copy or separate prepare task. History
and a partial five-symbol group survive frame boundaries. A full queue blocks
the mapper; the consumer wakes it after eight pages become free. Release/acquire
counters transfer ownership; physical cursors also survive counter wrap.

The fractional encoder uses priority 24, above Wi-Fi's task. It sleeps on queue
backpressure, giving other core-0 tasks time between encoder bursts. Legacy
priorities stay at 5. Interrupts remain enabled and timed. Only explicit
full-queue waits are excluded from encoder work and reported separately, like
legacy frame-slot waits. Sign extraction, framing and publication stay in MAP.

Round seven already prepared synchronously between stages. Its zero recorded
stage preparation deltas rule out that preparation as the explanation for the
inflated BCH/LDPC windows. The logs cannot identify the interrupt or task behind
the 351,164-cycle page stall. Round eight removes the old ring-read operation,
raises priority, and records maximum sign-extraction tick crossings and the
largest publication gap, with backpressure excluded. A recurring stall fails
admission; its source still needs a board measurement.

Legacy fills now notify the encoder after shaping and saving overlap. This
avoids waking SRAM-intensive encoder work before table loads finish. The
MODCOD 15 / 20m 41,356-cycle gate remains unchanged and requires remeasurement.

## Storage and samples

The existing 65,536 B shaper store holds
`24576 + 54*528 + 4096 + 8144 = 65328 B`: axis tables, guarded queue pages,
five-symbol sign LUT and encoder workspace. There are no packed frame slots,
dummy frame or producer window. The repeated input remains a 7,264 B internal
allocation during pace. Its 1,316 B copies are timed. DMA/parser/ISR input work
is outside this fixture. Active-rate overlap allocations and historical heap
floors are unchanged.

Each page holds 128 groups of five symbols, or 1,536 DAC samples. Full queue
coverage is `54*1536/80e6 s * 240e6 cycles/s = 248832 cycles`, versus 73,728
previously. The page owns its renderer's readable trailing word.

Sign bits are I-negative in 14:4 and Q-negative in 30:20, newest low. The three
four-sample vectors select chronological windows [-6..1], [-5..2], [-3..4].
The pulse and full-axis integer rounding are unchanged from wild-a f704aa0 and
moon-fill 73136ce. [host/machine.py](../host/machine.py) independently checks
complete convolution, including frame and bank seams.

## Admission

At 240 MHz and 80 MS/s, `15740/80e6 * 240e6 = 47220 cycles/bank`.
The 85% ceiling is 40,137 cycles/core/bank. A 33,282-symbol QPSK frame lasts
239,630.4 cycles at 100e6/3 symbols/s; its 85% ceiling is 203,685 cycles/frame.
Frame sizes come from host/dvbs2.py; geometry comes from radio_s3.h.

The coordinator checks complete core-1 bank maxima. Core-0 capacity uses fused
encoder mean/live maximum scaled by 47220/239630.4, plus measured wake and
publication allowances. Extraction is counted once inside encoder work.
The 4,823-cycle measured main SPI-copy mean remains a planning floor. It also
requires the measured publication gap plus wake allowance to fit
`((54-8)*1536-16380)*3 = 162828 cycles`: reserve after wake, charging one whole
bank burst. This checks slow-frame coverage independently of average throughput.

No queue starvation, underruns or lateness is allowed. Stack headroom must be
at least 512 B. All main timing ceilings and heap floors remain fixed. The
runner checks main first, continues after failures and exits nonzero.
After building/flashing the committed revision, with N0CALL and exclusive board
ownership:

```sh
PYTHON=/Users/river/radioconda/bin/python3 \
RX=/Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb \
  host/check.sh --moonshot notes/moon-round8-board
```

`--plan` opens no device. The runner never flashes, writes NVS or transmits.
