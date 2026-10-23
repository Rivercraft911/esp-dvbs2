# 8PSK at 26.67 Msym/s with a fresh payload

The transmitter now encodes 8PSK at 80/3 Msym/s (36 MHz channel) with a fresh xorshift
payload and at least 15% of each frame period to spare on core 0, at all three code rates:

| MODCOD | Rate | Payload | Encoder mean, before | Encoder mean, after | Reserve, before | Reserve, after |
|---|---|---:|---:|---:|---:|---:|
| 13 | 2/3 | 51.6 Mb/s | 187,638 | 159,471 | 6.1% | 20.2% |
| 15 | 5/6 | 64.6 Mb/s | 196,187 | 166,784 | 1.8% | 16.5% |
| 17 | 9/10 | 69.8 Mb/s | 180,195 | 157,218 | 9.8% | 21.3% |

Cycles per frame at 240 MHz; a frame is 22,194 symbols, 199,746 cycles, so 15% reserve means
a mean of at most 169,784. Before is f9207c4 (start of exp/tx-8psk), after is 796ec1c.
30 s radio-off paces, three runs after and two before: the means repeat to the cycle. After,
no run inserted a dummy frame (before, 5/6 had 2 startup and 2 steady dummies per run), no
fill was late, and the longest bank fill was 38,023 of 47,220 cycles (before: 38,654). Core 1
lost nothing. The longest single frame after warm-up was 164,684 / 172,392 / 162,434 cycles.

Every dumped DAC word equals the independent Python model: dumps of all three rates after
each change, and all 95 golden dumps at 8dd7508 and at 796ec1c.

Board: XIAO ESP32-S3 44:B1:76:B0:E5:54, radio off (`dvbpace`, `dvbdump`), Wi-Fi never
started, peak 128, a board reset before every pace. These are radio-off measurements; the
keyed confirmation belongs to the keyed table (results/2026-10-23-tx-table). A keyed `dvb`
run does not read the four cache counters at each stage mark, which costs dvbpace about
0.9k cycles a frame (measured with a cycle-only build), so keyed means should be lower.

## Where the cycles went

Per-stage means, cycles per frame, the 30 s paces above. "Map" starts the frame's mapping;
"Pages" maps the rest, frames it and writes the descriptor pages.

| MODCOD | Payload | BB | BCH | LDPC | Map | PL | Pages | Mean |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 13 before | 25,845 | 3,069 | 49,109 | 36,891 | 1,715 | 710 | 69,839 | 187,638 |
| 13 after | 21,142 | 2,478 | 41,744 | 31,772 | 1,076 | 175 | 60,981 | 159,471 |
| 15 before | 31,295 | 3,303 | 61,467 | 28,066 | 1,721 | 546 | 69,428 | 196,187 |
| 15 after | 26,040 | 2,908 | 52,065 | 23,523 | 1,099 | 177 | 60,868 | 166,784 |
| 17 before | 34,197 | 3,911 | 44,269 | 24,273 | 1,727 | 920 | 70,331 | 180,195 |
| 17 after | 28,060 | 3,079 | 43,897 | 19,836 | 1,067 | 175 | 61,002 | 157,218 |

Counters in a profiling build of f9207c4 (`prof` in runs.jsonl) split the page stage into
bulk mapping 17.4k, the backward PL move 9.8k, header and pilot inserts 1.9k, the first page
0.9k, descriptor preparation 26.2k, publication 5.4k and loop bookkeeping 6.8k. A second
build (`prof2`) split LDPC into u2 3.1-4.1k, rows 17.2/13.5/9.9k, transposes 6.4/3.2/3.2k and
parity packing 8.2/6.1/5.6k, and the payload into lane setup 1.8k plus 4.3 cycles a byte.

## Changes, in order

10 s paces, first of two runs (repeats agreed within 0.1%); encoder means in kcycles.

| Commit | What | 13 | 15 | 17 |
|---|---|---:|---:|---:|
| f9207c4 | start | 187.6 | 196.2 | 180.1 |
| bafcbf0 | lean 8PSK page loop; raw head from the framed words, no divide; helpers in IRAM | 183.3 | 193.2 | 174.3 |
| e878bf0 | t=10 BCH with the remainder one byte up: both indices from one EXTUI each, 14 instead of 17 instructions per byte pair (+4 KiB internal) | 176.2 | 184.1 | 174.3 |
| fcab374 | prepare kernel for aligned page sources | 173.6 | 181.5 | 171.7 |
| 4d3e172 | xorshift lane jumps by nibble tables (512 B) | 172.5 | 180.5 | 170.7 |
| 73f949e | unrolled LDPC parity pack for q = 18, 30, 60 (closed-form column offsets, immediate shifts; +2.3 KB IRAM, TX only) | 168.0 | 176.2 | 166.8 |
| b660723 | xorshift four lanes per vector, low bytes by six byte unzips | 164.9 | 172.4 | 162.6 |
| c33c4a6 | merge exp/tx-clean 60e722f (run memory, worker on core 1) | 164.8 | 172.1 | 162.6 |
| 8dd7508 | dumps wait for whole frames (below) | 164.9 | 172.2 | 162.6 |
| 479ca16 | stage marks in place: the encoder loop no longer copies 32-byte marks through 39 ROM memcpy calls (every mode, keyed too) | 162.3 | 169.6 | 160.1 |
| 2587493 | whole pages straight to the aligned prepare kernel | 161.0 | 168.3 | 158.8 |
| 2944ba8 | mapper combines its three phase-bit planes by Horner with saturating doubles | 160.3 | 167.6 | 158.0 |
| 6e20465 | backward PL move two vectors per pass, each source vector loaded once | 159.5 | 166.8 | 157.2 |
| 796ec1c | QPSK 80 MS/s keeps byte/pair BCH with SPI input (below); 8PSK unchanged | 159.5 | 166.8 | 157.2 |

Other modes (10 s paces, `dvbpace` set of results/2026-10-23-tx-budget, against
exp/tx-clean 60e722f): every other encoder mean fell, by 0.02% (VCM at 1 Msym/s) to 7.7%
(QPSK 8/9 short frames at 5 Msym/s; the xorshift kernel and the in-place marks serve all modes),
except VCM mixed at 5 Msym/s, +1.2% (365.8k to 370.2k of 1,597.5k, inside its known
run-to-run spread). Every longest bank fill moved by
between -1.4% and +0.5%. 8PSK 9/10 at 20 Msym/s, which this work did not target, stays
short of 15%: 254.3k of 266.3k (4.5%).

Memory: against 60e722f, IRAM text grows by 4,160 B (the unrolled LDPC packs 2,073 B, the
page loop, reader and prepare kernels) and .bss by 384 B (xorshift jump tables), so the end
of IRAM and the heap start move by one 4 KiB step. The t=10 BCH tables take 4 KiB more run
memory at 2/3 and 5/6 (37.9 KB low plus 18.9 KB high of 71.4 KB at 5/6).

## Dumps are deterministic

A radio-off dump is not real time, but its fill decided at each 8PSK 26m667 frame boundary
between data and a dummy frame by how far the encoder had got, so the dummy count changed
with encoder speed and from run to run (the pinned layouts held 8 dummies at 5/6 and 22 at
9/10; dumps on newer builds held 2 to 16). The dvbdump fill now waits for the whole next frame (8dd7508); keyed runs and
paces keep real-time admission. 13/15/17 dumps now always give the data-only CRCs of the host
model, 2,061,900,610 / 1,873,478,180 / 233,370,528 ([fixtures](../../tests/data/README.md)).

While the dummy layout still varied, notes/dumpverify.py found the layout from each dump and
compared every word with Python for that layout; it rejects a wrong callsign and a single
flipped bit. Its records are in dumps.jsonl.

## QPSK with SPI input

With `spiinput quad ts replay` (the real parser, CRC-32 and copies, Gate A), QPSK at 80 MS/s
fell back to nibble BCH. It now keeps the byte (t = 10/12) or pair (t = 8) kernels; at
33m333 the tables already live in the 8 KiB BCH cache, so this costs no RAM there.

| Replay input, dvbpace 10 s | Encoder mean, nibble (6e20465) | Encoder mean, 796ec1c | BCH, nibble | BCH, 796ec1c |
|---|---:|---:|---:|---:|
| `11 33m333` (239.6k frame) | 245,663 (stopped after 6 s) | 217,426 (9.3%) | 71,141 | 43,931 |
| `4 33m333` | 217,933 | 199,843 (16.6%) | 56,702 | 38,750 |
| `11 26m667` | 197,702 | 172,561 | 71,023 | 44,663 |
| `4 26m667` | 177,509 | 152,721 | 58,248 | 38,750 |

Replay dumps of these four modes are byte-identical between the two builds. The byte table
costs 8 KiB of run memory at 20m and 26m667 for t = 10/12; QPSK 1/4 at 20 Msym/s with replay
then takes 67.9 of 71.4 KB. QPSK 9/10 at
33.33 Msym/s still misses 15% with input: its ingress copy (41.3k) and TS BB (39.5k) remain.

## What did not help

- Reordering the BCH step so no result is read right after it is written: no change (52.6k),
  so the 1.6 cycles above 14 per pair are not latency stalls.
- Loading LDPC window offsets one window ahead: 5/6 and 9/10 gained 0.3k, 2/3 lost 0.1k
  (the extra load per level outweighs the saved stall when levels are short); not kept.
- Writing mapped words straight to their framed positions: the mapper already uses all eight
  vector registers, and the two-symbol shift of alternate frames costs about 7 instructions a
  vector in any form. Preparing descriptors straight from a shifted array would save an
  estimated 2.6k on those frames only (1.3k on average); not attempted.
- BCH three bytes per step: an estimated 3% to 5% fewer instructions for 8 KiB more
  tables, because 16-byte message feeds and 3-byte steps do not line up; not attempted.

## Files

`runs.jsonl` holds every pace; `commit` names the code (the `firmware` field is the git
description cmake cached at its last configure, so some builds repeat an earlier hash), and
`tag` the set. Intermediate commits keep the first of their two runs. `prof`/`prof2` are the
counter builds, `nocount` the cycle-only marks build, `sched`/`rows` the rejected changes,
`b0-clean`/`b0-final` the other-mode comparison, `replay-*` the input paces.
`dumps.jsonl` holds the dump checks: dumpverify records per commit, the replay dump CRCs and
the 95 golden dumps at 8dd7508 and 796ec1c. Tools: notes/matrix.py, notes/budget.py,
notes/dumpverify.py, notes/dumpcheck.py (W8's, local).
