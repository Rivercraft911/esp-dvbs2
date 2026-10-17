# Round two: a 20 Msym/s candidate, not a 26.67 Msym/s win

`20ms6` is implemented for **radio-off** `dvbdump` and `dvbpace`. Its six-symbol
pulse uses 16 KiB of tables. The assembled kernel models at **33,042 cycles per
16,380-word bank**, versus 46,443 measured for block4 at 3 sps. These are different
sample rates per symbol; this is not a 26.67 Msym/s performance comparison.
The complete 20 Msym/s fill models at **38,750 cycles**, or **40,107** carrying
forward the old fill model's unexplained error. The 15% limit is **40,137 cycles**.
That second estimate has only 30 cycles left. It is a candidate, not qualification.

`exp/max-opus` is merged at 8cb6337. The 128/512 geometry and interrupt policy are
used unchanged. Legacy waveforms retain their CRCs. No board, serial port or radio
was accessed. [Offline evidence](../results/2026-10-16-round2/README.md).

## Work removed

For phases `k`, levels `L[k]` in 2^-30 and taps `h` in 1/256 DAC count, each table
part is `floor((sum(L[k[a]] h[4a+p]) + 2^37) / 2^38)`, for three input symbols and
phase `p = 0..3`. Two triples cover the six-symbol pulse. Independent positive
biases sum to 512 on each axis, so adding two packed words and XORing `0x80200`
produces the signed 10-bit I/Q fields without carry between them. Exhaustive field
bounds cover peak settings 16, 128 and 508. `host/round2.py` independently derives
the pulse, tables and output; the encoder remains bit-exact to `host/dvbs2.py`.

The next phase-byte load runs between the current vector load and its consumer.
Each symbol needs eight instructions; a 24-symbol hardware loop adds one pointer
increment: `193 / 96 = 2.0104 cycles/sample` in steady state. The assembled model
includes setup, remainder handling and a conservative 32-cycle divide allowance.
Every load and output store uses all four lanes. All tails use PIE too.
Instruction timing and semantics: [S3 TRM v1.8](https://www.espressif.com/sites/default/files/documentation/esp32-s3_technical_reference_manual_en.pdf),
1.7.1 and Table 1.7-2, pp. 65-73; 1.8.75 p. 151 and 1.8.88 p. 164. The model is
single issue with dependencies, not QEMU timing, and excludes bus contention.

The fill retains only the 768 samples needed by the 80 MS/s geometry, including
the initial extra lead. It saves the final suffix once across the two callbacks
at a physical bank wrap. The static 2,176-word cache remains for 40 MS/s.
The 4,105-byte phase window borrows the fast encoder workspace after byte 8,144:
8,100 FEC bytes, alignment to 8,112, then a 32-byte guard. Fast stages never use
the reference encoder's parity array. The full workspace is still allocated.
QEMU checks all six 8PSK modes with changing tail contents and intact guards;
**plain LDPC must never run concurrently with this borrowed window**.

The new table leaves room for all three packed frames, dummy and payload inside
the existing 64 KiB store. Used bytes are 56,816 for 2/3 and 58,160 for 5/6.
The third frame's 11,120-byte heap allocation disappears. Fixed headers, pilots
and scrambling were already precomputed on main; no extra saving is claimed.

## Budgets and memory

One 20 Msym/s frame gives `240e6 * 22194 / 20e6 = 266328 cycles/core`.
Each bank gives `15740 * 240e6 / 80e6 = 47220 cycles`. The worst phase of the
819-bank wrap cycle has the following **model**, in cycles:

| Kernel | Unpack | Sample copies | Control assumption | Notification | Tick | Total |
|---:|---:|---:|---:|---:|---:|---:|
| 31,898 | 1,684 | 704 | 1,000 | 2,396 | 1,068 | **38,750** |

Copy cost is the existing PIE loop's 0.5 cycles/word. Unpack and control allowances
come from `host/fill_cycles.py`. Notification is the measured next2 maximum; the
tick is the inference in `docs/bank-geometry.md`, not a new measurement. Startup
models at 37,062 cycles. Carrying forward `43791 - (41366 + 1068) = 1357 cycles`
from the old steady-fill model gives 40,107. This is a sensitivity case, not a
worst-case bound. The scheduler's switch bookkeeping and actual minimum slack
still decide the pass. The new modulo calculation and memory placement need silicon.

At the 40,137-cycle fill gate, core 1 uses 226,379 cycles/frame. Core 0's prior
measured encoder means remain 167k/185k, with synthetic payload included. Both
fit a separate-core schedule with at least 15% reserve **if measured maxima pass**.
No new encoder speedup is assumed. Host MER is 45.01/45.02 dB without DAC hold;
the linear pulse with ideal hold has +0.230 dB mask margin and 42.82 dB pulse-only
MER. These do not qualify the quantized RF spectrum or 80 MS/s DAC bus traffic.

Linked sections against **aa75bab**, same SDK/config: IRAM text +1,688 B and
alignment +104 B; DRAM data -4,112 B; BSS unchanged. Net internal capacity saved:
**2,320 B**. Flash text +3,296 B, rodata +624 B. This includes the geometry merge
and the earlier block4 experiment. No extra task or stack is allocated.

| Mode | Requested internal allocation change | Predicted Wi-Fi-up free RAM |
|---|---:|---:|
| Legacy 4 sps | -2,320 B static, no new heap | 21.2 KB becomes about 23.5 KB |
| `20ms6` | above, plus -11,120 B frame | about **34.6 KB** |
| Legacy 8 sps | -2,320 B static, +2,058 B window | 21.1 KB becomes about **21.3 KB**; 25.5 KB becomes 25.7 KB |
| QPSK | -2,320 B static, +420 B window | 53 KB becomes about 54.9 KB |

KB means 1,000 B. New heap rows exclude allocator headers/alignment; rounded
baseline free-heap numbers are the coordinator's supplied measurements. These
are predictions against the 20 KB floor, not new Wi-Fi heap measurements.
Changing allocation addresses may affect encoder timing. `membench` additionally
uses 4,104 temporary bytes for this kernel, freed before command completion.

## Why 26.67 remains open

The new geometry requires **4.2301**, not 4.67, banks/frame:
`22194 * 3 / 15740`. Thus 49k fill plus 167k encoding costs 374,276 cycles/frame,
inside the two-core 399,492-cycle total, but with only 6.3% aggregate reserve.
A hypothetical split of 5k fill cycles on core 0 and 44k on core 1 leaves core 0
about `167000/4.2301 + 5000 = 44479 cycles/bank`. That fits only before job handoff,
preemption and jitter. The encoder's long BCH stage prevents treating it as a
cooperative per-bank helper. No such scheduler is implemented or timed here.

With the estimated 133.5k encoder (`167k - 43.5k + 10k` ingress), a 15% aggregate
reserve permits **48,715 fill cycles/bank**. With synthetic payload it permits
40,795. Neither an ingress measurement nor a RAM-qualified 72 KiB block4 stream
exists. Rescheduling does not establish either condition.

Quarter-turn symmetry does reduce block4 from **72 KiB to 18 KiB**, exactly:
for triple `(a,b,c)`, set `r = floor(a/2)` and subtract `2r mod 8` from every phase.
The representative has `2 * 8 * 8 = 128` possibilities. Rotate its signed I16/Q16
samples back by `r*pi/2`. Every table value reconstructs exactly in the host test.
Each triple has its own rotation, so undo must happen before summing triples.
The existing biased DAC words cannot simply be rotated with one scalar negate.

That representation loses the cheap packed-word sum. One straightforward PIE
route needs two `CMUL.S16` instructions per four-complex-sample rotation (each
instruction changes only two pairs), then four mask/shift/OR instructions to
pack each final vector into DAC fields. Even fusing table loads with rotations
leaves about 12 vector instructions per output vector in the all-rotated path,
versus six in block4, before normalization and rotation selection. At least
`(36 + 15)/12 = 4.25 cycles/sample` for that schedule loses. TRM 1.8.4-6 pp. 80-83.
This rejects that implementation, not every symmetry scheme or 26.67 Msym/s.
Antipodal folding halves memory but needs sign normalization and signed-sum
repacking; no faster assembled kernel was established.

At the existing 40/80 MS/s clocks, three samples/symbol is the only integer rate
above 20 Msym/s. I have no qualified faster fallback and no impossibility proof.
A fractional-sample pulse would require its own kernel, signal tests and timing.
The next useful experiment is measured SPI DMA ingress plus a compact n3 kernel,
not a scheduler whose total-work and memory conditions remain unproved.

## Coordinator, radio off

The gates below are fixed before the run. Build/flash through the coordinator's
normal process. `20ms6` is rejected by keyed and Wi-Fi-probe parsers. Each command
is at most about a minute. Use a new folder. CRCs require the already-stored
**N0CALL**; another callsign needs matching host expectations, not an NVS change.

```sh
PY=/Library/Frameworks/Python.framework/Versions/3.13/bin/python3
OUT=notes/round2-board
RX=/Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb
mkdir "$OUT"
"$PY" host/board.py info --reset --out "$OUT/runs.jsonl"
"$PY" host/board.py membench --timeout 75 --out "$OUT/runs.jsonl"
"$PY" host/shaper.py verify "$OUT/runs.jsonl"
"$PY" tests/test_maxrate.py --membench "$OUT/runs.jsonl"
"$PY" host/board.py 'fecbench pie' --timeout 75 --out "$OUT/fec.jsonl"
"$PY" tests/test_fecbench.py "$OUT/fec.jsonl" --correctness-only
"$PY" host/board.py 'dvbdump 4 40' --timeout 75 --save "$OUT/4.iq" --out "$OUT/runs.jsonl"
for mc in 13 15; do
    for rate in 5m 10m 20ms6; do
        "$PY" host/board.py "dvbdump $mc $rate 40 128 1" --timeout 75 \
            --save "$OUT/$mc-$rate.iq" --out "$OUT/runs.jsonl"
    done
    "$PY" host/onair.py "$OUT/$mc-20ms6.iq" --rate 80000000 --symbol-rate 20000000 \
        --modcod "$mc" --stream 40 --callsign N0CALL --scale 512 --mer --shift 0 --receiver "$RX"
    "$PY" host/board.py "dvbpace $mc 20ms6 30" --timeout 60 --out "$OUT/runs.jsonl"
done
"$PY" tests/test_round2.py --board "$OUT/runs.jsonl"
```

Correctness first: every legacy membench checksum; all 140 FEC CRCs and 14 zero
PIE mismatches; old dumps respectively 1435641601, 1937985440, 474263962,
1163322913, 1757765864. New 13/15 dumps: **3178964763 / 4256019779**;
all overlap mismatches zero. Decode each new dump to at least 30 contiguous frames,
zero payload errors, LDPC flips/check failures and BCH failures, MER >=38 dB.
The short kernel's C and PIE FNV checks must both be **3853495837**.

Timing gates: `short4_pie.cycles_min <=33750`; both new paced runs `ESP_OK`, no
encoder failure, underruns, late fills, late switches or fatal lateness;
`max_fill_cycles <=40137`, **`min_fill_slack_cycles >=7083`**, and
`encode_cycles_live_max <=226378`. Keep startup maxima in the record; only the
encoder's explicitly reported prefill frames are excluded from its live maximum.
Require 80,000,000 samples/s, step 15,740, three ring slots, zero ring heap bytes,
and `shaped_words + reused_words == fill_words`. A failed timing gate rejects the
15% claim even if the receiver decodes. Radio-off tests cannot establish DAC-read
contention, actual Wi-Fi-up heap, or analog compliance.
