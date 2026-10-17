# Two-core experiment

**None of the three requested roll-off 0.20 modes closes the current model.**
The complete host streams are correct. Silicon timing and Wi-Fi memory remain
unmeasured. No serial port, board, flash or radio was accessed.

The encoder from `910da2d` and kernels from `d655793` are merged. `33m333`
selects exactly 100/3 Msym/s and 12/5 samples/symbol for QPSK/8PSK. 8PSK
`26m667` selects 80/3 Msym/s and 3 samples/symbol. All three now select the
block kernels, BBHEADER RO=10 and pulse roll-off 0.20. The portable FIR is gone.
The occupied bandwidth passed to the filter selector is 40 MHz or 32 MHz.
These are experimental selections, not admitted real-time modes.

The supplied 3-sps kernel used a six-symbol pulse at **0.35**, not 0.20.
Changing its alpha alone fails the pulse model. Its new nine-symbol 0.20
variant needs four table parts and 51 model cycles per twelve samples,
versus 31 for the old pulse. The quantized pulse/hold/alias model passes
Annex A; the analog radio still needs a cabled spectrum measurement.
Sources: `host/moon_fill.py`, EN 302 307-1 V1.4.1, 5.6 p35 and Annex A p38.

## Ownership and schedule

Core 1 copies a bank's symbols into an immutable window, then publishes one
job with release ordering. A priority-6 task on core 0 preempts the priority-5
encoder. Both cores pack their own input and shape disjoint sample ranges.
Their separate staging regions borrow the unused encoder workspace tail.
Each chunk retains two input blocks of history. The helper observes publication
with acquire ordering and release-stores completion. Core 1 joins before
reusing the window, saving overlap samples or returning the bank. Ring slots
can be reused after the window copy. Only core 1 accesses the ring's read state.

There is one job slot, no queue or general scheduler. Fixed helper fractions
are 2/7 for 8PSK 3 sps, 1/3 for QPSK 2.4 sps and 1/5 for 8PSK 2.4 sps,
rounded to twelve-sample boundaries. A physical wrap can need two jobs.
The helper wakes the encoder locally after ring release, avoiding another
cross-core interrupt. An unusually short piece runs locally.

Telemetry records dispatch, helper body, helper work per complete physical
bank, join wait and the original full-bank wall time. Encoder wall intervals
remain available; `encode_work_cycles_*` subtracts preempting helper bodies
on the same core. Counters from different cores are never subtracted.

## Capacity, not a worst-case latency proof

At 240 MHz/core and 80 MS/s, a fresh bank has 15,740 samples:
`B = 15740 * 240e6 / 80e6 = 47220 cycles`, or 196.75 us.
The 15% reserve ceiling is `0.85 B = 40137 cycles/core/bank`.
Normal pilot-bearing frames have 22,194 8PSK or 33,282 QPSK symbols
(EN 302 307-1, 5.5/Table 11). `F = symbols * 240e6 / Rs` cycles/frame.

Even with a freely divisible fill and zero overhead, the two-core sum permits
only `2*40137 - E*B/F` fill cycles/bank: 56,287 (8PSK 26m667),
60,610 (QPSK 33m333), 50,290 (8PSK 33m333). This is a necessary bound,
not an available single-core deadline.

For encoder work E cycles/frame, helper work H and local work L cycles/bank:

```
core 0 demand = E*B/F + H + scheduling + tick
core 1 time   = max(H,L) + ring staging + overlap copies + dispatch + tick + control
```

`host/moon_schedule.py` enumerates all 819 physical bank offsets. It replays
assembled kernel and packing loop instructions through the existing LX7
issue model. The handoff probe executes 32 real FreeRTOS notifications in
QEMU: 340..909 instructions/dispatch, at most 1,331 issue cycles including
redirects but excluding interlocks. QEMU CCOUNT is never a timing input.
The model reserves 2,396 cycles/job on each core, rounded above the earlier
2,394-cycle stream notification maximum in
`results/2026-10-17-qpsk-goal/runs.jsonl`. It assumes one cycle/staged byte,
half a cycle/copied sample word, 1,068 tick cycles/core and 1,000 control cycles.
These allowances are estimates, not bounds on Wi-Fi/interrupt/cache/SRAM costs.

| Mode, 9/10 | F, cycles | E, cycles/frame | Core 0, cycles/bank | Core 1, cycles/bank | Kernel reduction needed with this split |
|---|---:|---:|---:|---:|---|
| 8PSK 26m667 | 199,746 | 101,466 | 56,242 | 78,155 | 51 to <=8.05 cycles/block, 84.2% |
| QPSK 33m333 | 239,630.4 | 99,787 | 51,070 | 59,233 | 48 to <=22.98 cycles/block, 52.1% |
| 8PSK 33m333 | 159,796.8 | 101,466 | 57,860 | 100,869 | Kernel alone cannot suffice |

E comes from `results/2026-10-17-moon-encoder/runs.jsonl`, including its
estimated 10,000-cycle input copy. It excludes the test payload generator,
station-header insertion, cache contention and stream bookkeeping. The board
paces retain and time the real generator; they do not replace it with this
estimate. Packing adds 18, 7.75 or 21 issue cycles/block respectively.
With zero kernel work, 8PSK 33m333 still uses 41,878 cycles on core 0,
1,741 over the ceiling. Packing, handoff or the split must change too.
The reductions above require different kernel factoring; they are not
predictions that instruction scheduling alone can achieve them.

The model is a capacity rejection, not a claim of periodic 15% idle time.
Even a passing capacity model would require measured deadline/latency checks
with Wi-Fi up. No encoder stages are pipelined into a second workspace.

## Memory and correctness

The 65,536-byte store and the existing internal encoder workspace/context
remain. Block input staging uses 2,740 bytes or less of the workspace tail.
The helper requests a 2,048-byte stack plus its task control block.

| Mode | Tables, B | Extra table heap, B | Ring/window heap, B |
|---|---:|---:|---:|
| 8PSK 26m667 | 98,304 | 49,152 | 29,104 |
| QPSK 33m333 | 37,056 | 0 | 0 |
| 8PSK 33m333 | 98,688 | 49,536 | 29,104 |

These are requested sizes, not measured free heap. The closest cleaned-main
9/10 layout had 47,291 bytes free with Wi-Fi up
(`results/2026-10-17-cleanup/runs.jsonl`, `wifimem 11 26m667`). The 8PSK
extra requests alone exceed that whole headroom. They need at least 50,965 /
51,349 bytes reclaimed to reach a 20,000-byte floor, **before** new linked RAM,
the helper stack, task control block and allocator overhead. Faster tables must
also be smaller. QPSK fits the shared store; its Wi-Fi heap is still unmeasured.

All 57 retained waveform CRCs and the encoder's extra fixture pass. Six new
16-frame C streams match the independent Python model sample for sample,
including frame seams and full bank-wrap cycles. LeanDVB recovers 14 contiguous
QPSK frames and 13 8PSK frames with zero FEC corrections or payload errors.
`tests/data/moon_stream_crcs.json` records these new waveforms. The S3 kernels
pass 324 QEMU block cases and 15,552 guarded sample slices.

## Coordinator

After building/flashing the committed revision, with N0CALL already stored:

```sh
host/check.sh --moonshot --plan
PYTHON=/path/to/python-with-numpy RX=/path/to/leandvb \
  host/check.sh --moonshot notes/moon-board
```

Criteria print first. Six 16-frame dumps cover all targets and three legacy
paths; CRC/length/overlap and LeanDVB gates remain strict. Five paces cover
three 30-second targets plus two regressions. Target paces keep Wi-Fi NULL/
promiscuous receive up without TX keying, retuning, DAC or bank grants. They
check the 40,137-cycle bank ceiling, encoder frame limits, helper/encoder
capacity screen, dispatch <=2,396 cycles and zero underruns/late switches.
Four `wifimem` probes cover all targets and the tight legacy mode. Targets
require free-up and low-water >=20,000 bytes. Existing regression floors stay.
Every failed stage is reported and later stages continue. The script never
flashes or keys RF. The current targets are expected to fail timing/RAM.

Host reproduction: `tests/test_rational.py --receiver /path/to/leandvb`.
Instruction-model reproduction and hashes are in the two-core results record;
raw traces and the longer analysis remain in ignored `notes/moon-sys/`.
