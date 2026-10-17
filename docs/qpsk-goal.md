# QPSK at 26.67 Msym/s, integrated and awaiting silicon

The encoder, three-frame ring and block fill now run together through `dvbdump`,
`dvbpace` and `dvb`. `26m667` selects QPSK at exactly 80/3 Msym/s, not a rounded
26,667,000 Hz clock. `20mq4` selects the new four-sample kernel at 20 Msym/s.
Both accept MODCODs 4..11. All existing rate tokens retain their samples.
No serial port was opened, firmware flashed, Wi-Fi started or radio keyed here.

The new four-sample kernel changes rounding and peak normalization. Silently
selecting it for `20m` would break two of the eighteen required CRCs. Therefore
`20m` retains its original waveform; `20mq4` is explicit. The coordinator compares
both full paths before deciding whether to adopt the new waveform as a default.
The fill-only measurements alone do not establish that decision.

## Pass criteria, before the board run

Run `bash host/qpsk-goal-board.sh` on the coordinator's clean, separately flashed
checkout. This script never flashes, keys, changes callsign or writes NVS. Its
only board commands are `info`, `fecbench`, `dvbdump`, `dvbpace` and `wifimem`.
The script prints all gates before running, saves all output and continues through
failures. It exits nonzero if any of its 56 command/check/decode results fail.
It requires stored `N0CALL` and the checkout's firmware revision.

1. `fecbench pie`: all 140 reference CRCs and 14 zero-mismatch PIE checks.
2. The eighteen old dumps below and four new dumps: `ESP_OK`, healthy encoder,
   zero underruns and overlap mismatches, exact byte count, matching firmware,
   USB and saved-file CRC. LeanDVB must recover at least frames-2 contiguous,
   bit-exact payload frames, with zero LDPC checks/flips, BCH failures/corrections
   and payload errors. New raw-sample MER must be >=37 dB at 3 sps, >=40 dB at 4 sps.
3. The six paces below, each 30 s: full live encoder (`encoder_timed:true`), no
   injected stall, zero underruns, late fills, late switches or fatal lateness.
   Require the expected mapper, ring slots, memory placement and profile totals.
4. `wifimem 11 26m667`, `wifimem 17 5m`, then the two worst new-layout probes,
   `wifimem 8 26m667` and `wifimem 8 20mq4`. Both free-up and minimum internal
   heap must exceed 30,000 B at 26m667 or 20,000 B otherwise. Require `ESP_OK`,
   encoder prefill only, zero played frames/positions/fill calls. These are Wi-Fi
   NULL-mode startup/shutdown checks, without playback, keying or retuning.

| `dvbpace` arguments | Max fill, cycles | Min slack, cycles | Encoder mean / live max, cycles/frame |
|---|---:|---:|---:|
| `11 26m667 30` | 40,137 | 7,083 | 254,607 / 254,607 |
| `4 26m667 30` | 40,137 | 7,083 | 254,607 / 254,607 |
| `11 20m 30` | 46,769 | 150 | 188,283 / 192,748 |
| `13 20ms6 30` | 41,318 | 5,600 | 172,438 / 174,975 |
| `17 5m 30` | 72,766 | 12,842 | 905,515 / 905,515 |
| `11 20mq4 30` | 40,137 | 7,083 | 339,476 / 339,476 |

The legacy 20m and 20ms6 gates are unchanged from next5. There is no supplied
17/5m paced baseline: its gates reserve 15% of the bank and frame budgets, not a
claim that it runs at those ceilings. For the new 4-sps kernel, also compare the
observed maximum directly with `11 20m`; adoption requires an actual improvement.
No prefill encode maximum is mistaken for the steady live maximum.

| `dvbdump` arguments | Frames | IQ CRC32 |
|---|---:|---:|
| `4 40` | 40 | 1435641601 |
| `13 5m 40 128 1` | 40 | 1937985440 |
| `15 5m 40 128 1` | 40 | 1163322913 |
| `13 10m 40 128 1` | 40 | 474263962 |
| `15 10m 40 128 1` | 40 | 1757765864 |
| `13 20m 40 128 1` | 40 | 474263962 |
| `15 20m 40 128 1` | 40 | 1757765864 |
| `13 10m80 40 128 1` | 40 | 1937985440 |
| `13 20ms6 40 128 1` | 40 | 3178964763 |
| `15 20ms6 40 128 1` | 40 | 4256019779 |
| `4 5m 40` | 40 | 3933043456 |
| `11 5m 40` | 40 | 1552954365 |
| `4 10m 40` | 40 | 2594131925 |
| `11 10m 40` | 40 | 2779934507 |
| `4 10m80 40` | 40 | 3933043456 |
| `11 10m80 40` | 40 | 1552954365 |
| `4 20m 40` | 40 | 2594131925 |
| `11 20m 40` | 40 | 2779934507 |
| `4 26m667 16 128 1` | 16 | 3388792897 |
| `11 26m667 16 128 1` | 16 | 1271511239 |
| `4 20mq4 16 128 1` | 16 | 3789273049 |
| `11 20mq4 16 128 1` | 16 | 2081653259 |

The script fixes Python to `/Library/Frameworks/Python.framework/Versions/3.13/bin/python3`.
`RX` may override its local LeanDVB executable. A manual new-mode decode is:

```sh
PY=/Library/Frameworks/Python.framework/Versions/3.13/bin/python3
"$PY" host/onair.py notes/<run>/11-26m667.iq --rate 80000000 \
  --symbol-rate 26666666.666666668 --scale 512 --modcod 11 --stream 16 \
  --callsign N0CALL --mer --shift 0 \
  --receiver /Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb
```

## Data path

QPSK's two phase bits already exist in the encoder's bit-plane expression.
`DVBS2_MAP_QUADS` stores them directly, high pair first, four symbols per byte.
The PIE pass uses the old index calculation without widening into halfwords or
performing table lookups. A word pass inserts the half-byte offset after the
90-symbol header; PL framing overwrites the header/pilot halves. The last byte's
unused low bits are zero. Payload temporarily occupies context scratch, consumed
by BB framing before BCH/LDPC and the mapper reuse it.

A PLFRAME has 33,282 symbols, and a dummy has 3,330: neither ends on a four-symbol
boundary. The ring retains its global symbol timeline and copies packed slices
with the required two-bit shift. Aligned PIE copies handle the bulk. Scalar edges
preserve neighboring codes. Choices and releases use the same acquire/release
ordering as the old ring; 12..15 symbols of retained history occupy four bytes.
Only needed future symbols are read, so rounding a block cannot choose a dummy
prematurely. Unused symbols in a final block have zero taps for earlier samples.

The existing sample-overlap cache and notification path remain shared. The new
fill window borrows the fast workspace tail (1,369 B at 3 sps, 1,028 B at 4 sps).
All PIE workspaces, contexts, ring slots, tables and vector destinations are
16-byte aligned. The mixed byte/vector copy handles arbitrary source alignment
and keeps sixteen readable guard bytes. The prior malloc-alignment failure is
not repeated.

## Timing: measurements versus planning allowances

The [fill-only board run](../results/2026-10-17-qpsk-fill-board/README.md) measured
34,584/32,247 cycles at 3/4 sps. It used pre-encoded input and is not evidence of
integrated timing. The supplied QPSK encoder measured about 155k..185k cycles per
frame with the old nibble mapper. Its 9/10 map/PL stages averaged about 25.8k/2.2k
cycles in `board-8437931-pace.jsonl` (recorded provenance in next5's results).

Planning allowances, not predictions validated on silicon:

- Fill: isolated maximum + 2,348-cycle prior notification maximum + 2,000 cycles
  for ring stitching, profiling and contention: **38,932 / 36,595 cycles**.
  The last 2,000 cycles is an assumption; a worse concurrent SRAM/cache result
  can erase the margin. The new bit-copy loop adds work to the isolated probe.
- Encoder: allow 72k cycles for the new map and 6k for PL instead of the prior
  25.8k/2.2k. `185k - 25.8k - 2.2k + 72k + 6k = 235k cycles/frame` at 9/10.
  These are deliberately provisional allowances for the assembled PIE plus
  scalar word-copy path, not QEMU timing or a measured speedup.

`15,740 samples * 240 MHz / 80 MHz = 47,220 cycles/bank` and
`0.85 * 47,220 = 40,137 cycles`. At 3 sps the frame budget is
`33,282 * 3 * 240/80 = 299,538 cycles`; the extra encoder gate reserves 15% too.
Payload is `(58,192-80) * (80e6/3) / 33,282 = 46.561 Mb/s`.
The brief's earlier 57.2k fill and 262k encoder refer to 8PSK; they are context,
not a like-for-like QPSK baseline. Instruction semantics follow the TRM citations
in [qpsk-fill.md](qpsk-fill.md) and the assembly comments; no new silicon
instruction-latency claim is made here.

## Internal RAM, every layout

All bytes below are derived allocation sizes, not measured free heap. Every old
mode retains its ring, workspace, BCH policy, payload placement and window size.
They all recover **3,328 B** of linked internal capacity versus next5, or **768 B**
versus main `4f926aa`. Wi-Fi is not reconfigured. Thus the modeled Wi-Fi-up and
minimum-heap deltas are +3,328 B for every existing MODCOD/rate, before allocator
alignment/order effects; no old mode pays new per-command heap.

| Linked section, B | next5 `298235f` | Integrated | Change |
|---|---:|---:|---:|
| IRAM vectors | 1,028 | 1,028 | 0 |
| IRAM text | 96,491 | 93,119 | -3,372 |
| IRAM end alignment | 17 | 61 | +44 |
| DRAM data | 30,984 | 30,984 | 0 |
| DRAM BSS | 80,592 | 80,592 | 0 |
| Flash text | 504,176 | 515,044 | +10,868 |
| Flash rodata | 242,428 | 243,628 | +1,200 |

Plain encoder and portable fallback routines, TS BB framing, the staged-row
experiment and the 5-sps benchmark now execute from flash. None is in the live
GCS/PIE path for any supported transmit mode. Their old standalone timing
comparisons are invalidated; their output is unchanged. Live kernels stay in
IRAM. Do not count `.dram0.dummy` again: it aliases IRAM.

The 65,536 B table store and 131,072 B DAC banks already exist. No new persistent
buffer or task stack was added. Mode-specific internal heap is:

| MODCOD | Context | Legacy QPSK 40 sps | QPSK old fast / `26m667` | `20mq4` | 8PSK 4 sps | 8PSK 8 sps | `20ms6` |
|---|---:|---:|---:|---:|---:|---:|---:|
| 4 | 16,816 | 17,236 | 37,428 | 50,064 | n/a | n/a | n/a |
| 5 | 16,816 | 17,236 | 37,428 | 50,064 | n/a | n/a | n/a |
| 6 | 16,816 | 17,236 | 37,428 | 50,064 | n/a | n/a | n/a |
| 7 | 16,816 | 17,236 | 37,428 | 50,064 | n/a | n/a | n/a |
| 8 | 17,488 | 17,908 | 38,100 | 50,736 | n/a | n/a | n/a |
| 9 | 16,816 | 17,236 | 37,428 | 50,064 | n/a | n/a | n/a |
| 10 | 17,488 | 17,908 | 29,908 | 42,544 | n/a | n/a | n/a |
| 11 | 17,680 | 18,100 | 30,100 | 42,736 | n/a | n/a | n/a |
| 12 | 15,568 | n/a | n/a | n/a | 47,300 | 46,388 | 36,180 |
| 13 | 15,184 | n/a | n/a | n/a | 46,916 | 46,548 | 35,796 |
| 14 | 16,624 | n/a | n/a | n/a | 48,356 | 48,659 | 37,236 |
| 15 | 16,528 | n/a | n/a | n/a | 48,260 | 49,242 | 37,140 |
| 16 | 17,488 | n/a | n/a | n/a | 41,028 | 50,656 | 29,908 |
| 17 | 17,680 | n/a | n/a | n/a | 41,220 | 50,938 | 30,100 |

These exclude allocator headers/alignment and common RTOS/radio/Wi-Fi costs,
including the existing 4,096 B encoder stack. The context is unchanged. BCH is
8,192 B for t=10/12 at internal-workspace rates, zero for t=8. QPSK's default
workspace stays in PSRAM; 8-sps 8PSK also keeps its PSRAM workspace. Old table-store
occupancies and PSRAM requests are exactly [next5's per-mode account](next5.md).
Wi-Fi's common allocations remain those in the supplied board records; their
actual per-mode free-up/minimum cannot be established offline.

New layouts, independent of code rate because payload borrows context scratch:

| Store contents, B | `26m667` | `20mq4` |
|---|---:|---:|
| Table | 36,864 | 49,152 |
| Three frames (3 * 8,352, guards/alignment included) | 25,056 | 0 |
| Workspace | 0 | 12,420 |
| Alignment after workspace | 0 | 12 |
| Dummy (guards/alignment included) | 864 | 864 |
| Used / unused | 62,784 / 2,752 | 62,448 / 3,088 |
| Ring heap | 0 | 25,056 |
| Workspace heap | 12,420 | 0 |

The 3-sps heap equals existing fast QPSK. The 4-sps heap adds 12,636 B, rather
than 16,704 B with one frame in the store and a heap workspace. It remains an
explicit new waveform, not a silent cost to existing `20m`. Both keep resident
BCH and three ring slots. Sharing payload with scratch avoids another
4,016..7,264 B per new mode without changing context size.

Wi-Fi estimates using the nearest supplied board layout:

| Probe | Prior measured up / minimum, B | Estimated integrated up / minimum, B | Gate |
|---|---:|---:|---:|
| `17 5m`, main | 21,579 / 21,579 | 22,347 / 22,347 | 20,000 |
| `13 20ms6`, main | 36,727 / 34,875 | 37,495 / 35,643 | existing 30,000 |
| `11 26m667`, from QPSK `11 5m` | 37,307 / 37,019 | 43,211 / 42,923 | 30,000 |
| `8 26m667`, from QPSK `8 10m` | 29,295 / 27,479 | 35,199 / 33,383 | 30,000 |
| `8 20mq4`, same reference | 29,295 / 27,479 | 22,563 / 20,747 | 20,000 |

The QPSK reference build has 4,112 B more static data and 1,792 B more aligned
IRAM than this image: +5,904 B before per-command changes. Other new QPSK code
rates have the exact heap requests above; their Wi-Fi measurements are unknown.
Allocation order, alignment and startup transients still need silicon, especially
the 747 B predicted reserve in the worst new 4-sps layout. Sources are next5's
provenance records and [main's heap measurements](../results/2026-10-17-next4/heap.jsonl).

Rejected for this integration: compressing table rows through 180-degree symmetry
would add sign/bias work to each timed block and needs a new kernel measurement.
The table already fits; sharing the workspace and moving unused hot placement
recovers RAM without changing a shaping word. Converting a nibble ring inside
the fill would spend the scarce fill budget; emit its actual two-bit format instead.

## Offline checks and remaining gate

All 34 host test programs pass. The four host dump MERs are 37.995/37.984 dB
at 3 sps and 41.219/41.206 dB at 4 sps (MODCODs 4/11). Native stream tests cover all eighteen old CRCs,
all sixteen new MODCOD/rate pairs, allocation failure at every request, repeated
commands and complete physical-wrap cycles. Forced starvation tests preserve
choices and dummy transitions at both bank geometries, including two threads.
LeanDVB recovers 14 contiguous bit-exact frames from each new 16-frame dump and
38 from each of the 32 old fast-QPSK configurations, with zero corrections.

QEMU checks 140 FEC CRCs, 14 PIE checks, 896 existing packed encoder cases,
512 new quad cases with borrowed payload and guarded workspace tails,
65,536 quad-copy slices, 19,368 block-kernel slices and the existing shaper/glue
checks. QEMU proves instruction behavior, not timing. Script tests inject a
command error, bad CRC, decode failure and low heap; all later commands still run.

IDF v6.1 builds. The exact activation/build command initially hit sandbox-denied
`psutil`/`sysctl` discovery in the unused component manager;
`IDF_COMPONENT_MANAGER=0 idf.py build` succeeds without project configuration
changes. Reproduction instructions remain in the encoder/shaper QEMU READMEs.
Records: [offline results](../results/2026-10-17-qpsk-goal/README.md).

The goal is not yet demonstrated on silicon. Next: run the complete radio-off
script, inspect the worst-bank profile and Wi-Fi minima, then improve the
measured bottleneck if any gate fails. No RF test is part of this handoff.
