# 40 MHz system experiment

`33m333` runs the complete encoder, ring and bank timeline at exactly 100/3
Msym/s, 12/5 samples/symbol, roll-off 0.20. QPSK and 8PSK accept it in `dvbdump`,
`dvbpace`, `wifimem` and `dvb`. 8PSK also accepts `26m667` at 3 sps, roll-off 0.35.
**These new modes use a portable FIR correctness path. They are not real-time.**
No board was opened, reset, flashed or keyed for this experiment.

The actual C stream's samples equal the independent Python encoder/convolution.
With N0CALL, peak 128 and 16 frames, the local LeanDVB recovered 14 QPSK 9/10
frames and 13 8PSK 9/10 frames at 33m333, and 12 8PSK 9/10 frames at 26m667.
All recovered payloads were contiguous and exact, without BCH/LDPC corrections.
`tests/test_rational.py --receiver /path/to/leandvb` reproduces this host result;
it is not board or RF evidence. The 57 existing waveform CRCs are unchanged.
New CRCs are in `tests/data/rational_crcs.json`.

## Timeline, pulse and interfaces

At sample n, symbol index is `floor(n*den/num)` and fractional phase is
`(n*den)%num`. Ring reads use sample coordinates multiplied by den and sps=num;
this preserves ring ownership and dummy choices without rounding the symbol rate.
A dump contains `ceil(frames * frame_symbols * num/den)` samples. Rounding each
frame separately would insert samples and break the continuous timeline.
`DVB_SPS_12_5` names the special numerator accepted by the existing transmitter
entry points. Telemetry reports numeric `sps=2.4` plus exact `sps_num=12,sps_den=5`.

`DVBS2_RO_20` in fast preparation selects MATYPE-1 RO=10 in GCS BBHEADERs,
including their CRC and scrambling. The plain `dvbs2_bb_gcs_ro` is the stage
oracle. Defaults retain RO=00 and every old CRC. PL headers do not signal RO.

`host/rational.py` integrates RRC(f)/sinc(f/Fs), then applies a Kaiser window
(beta=1), truncates to 12 symbols and quantizes coefficients to Q15 DAC LSB.
Frequency f is in cycles/symbol, Fs/Rs=12/5. It scales the worst sum of absolute
coefficients to 126 LSB at peak=128. The C FIR rounds once per output axis.
The coefficient/alias-power/ideal-hold model has Annex A upper/lower/far margins
+0.187/+0.250/+8.014 dB. This excludes DAC quantization noise and the analog
radio. A board dump and cabled RF spectrum must check those effects.
Source: EN 302 307-1 V1.4.1, 5.6 p35, Annex A Table A.1 p38.

The fill replacement interface is `shaper_rational` in `shaper_rational.h`:
canonical 16*phase bytes with history, rational phase, exact output count and
caller-owned taps. It has no allocation or peripheral access. The encoder still
uses the existing six stage APIs. Kernel changes must also update the independent
waveform model when rounding/pulse design changes, preserving all old CRCs.
`fill_kernel=portable-fir` makes the current selection explicit.

Wide-filter support is merged from exp/widefilter. The occupied support passed
to `radio_s3_run_bandwidth` is exactly 40,000,000 Hz for 33m333 and 36,000,000 Hz
for 26m667. Auto selects mode 2 after CW setup. The selector/readback tests pass;
[RF width is unmeasured](wide-filter.md). Radio-off commands do not touch it.

## Budgets and two cores

Inputs: 240 MHz/core, 80 MS/s DAC, STEP=15,740 samples, normal frames with
pilots: 22,194 symbols (8PSK), 33,282 (QPSK), EN 302 307-1 5.5/Table 11.
`B = STEP * 240e6/80e6 = 47,220 cycles/bank` (196.75 us).
`F = frame_symbols * 240e6/Rs` cycles/frame. Reserve target is 15%.

| Mode | Frame period, cycles | 85% encoder ceiling, cycles | Bank ceiling, cycles |
|---|---:|---:|---:|
| 8PSK 26m667 | 199,746 | 169,784 | 40,137 |
| 8PSK 33m333 | 159,796.8 | 135,827 | 40,137 |
| QPSK 33m333 | 239,630.4 | 203,685 | 40,137 |

The portable FIR cannot meet B: the linked LX7 inner loop issues at least
25 instructions/tap/sample. At 12 taps and 15,740 fresh samples that alone is
4,722,000 issue cycles, before loop setup, stores, divisions or stalls. At 9 taps
it is 3,541,500. These are instruction-count lower bounds, not QEMU or silicon
timing predictions. New pace gates are expected to fail until a fast fill lands.

For encoder cost E cycles/frame, fill cost C cycles/bank and work X moved to
core 1, a necessary capacity screen is

```
core 0: E-X <= 0.85 F
core 1: C + X B/F + handoff_cost <= 0.85 B
therefore E/F + C/B <= 1.70 before handoff overhead
```

This checks utilization, not release times or worst-case latency. Main's QPSK
9/10 live maximum is 228,865 cycles/frame; at 33m333 at least 25,180 cycles/frame
must move or disappear to reach 15% reserve. Its measured payload mean is
58,763 cycles/frame. Moving all payload work leaves at most 28,557 fill cycles
per bank, before handoff overhead. Source: `results/2026-10-17-qpsk-goal/runs.jsonl`,
`dvbpace 11 26m667 30`. Whole-stage maxima are not added across different frames.

For 8PSK 9/10 there is no internal-workspace live measurement at these rates.
An optimistic planning case combines the isolated hot FEC frame (119,955 cycles,
including BCH 70,377) in `results/2026-10-17-cleanup/runs.jsonl`, `fecbench pie`,
MODCOD 17/GCS/frame 0, with the measured 5m payload mean (58,765 cycles) from
qpsk-goal. This gives E=178,720, **an estimate, not a live prediction**. Other
fecbench frames were much slower. At 33m333:

- Arbitrary divisible offload needs >=42,893 cycles/frame; C must be <=27,462.
- Whole payload offload needs C<=22,771; core 0 then uses 119,955/F=75.1%.
- Whole BCH offload needs C<=19,340; core 0 then uses about 67.8%.

Moving a serial stage alone does not shorten a frame. Pipeline adjacent frames
with two workspaces and retain each output slot until publication. Fill must
have priority; the helper may consume only a bounded chunk after each bank fill,
then release-store completion. One full BCH call cannot fit one bank's slack.
A chunked BCH interface needs private recurrence state and a second 12,420 B
workspace; overlapping fast stages on one workspace/context violates today's API.
Payload pipelining can instead borrow an unpublished output slot, but must prove
slot release, xorshift state continuity and chunk deadlines. No unmeasured helper
scheduler is enabled. Splitting fill onto the encoder core helps only after its
own excess demand is removed; spare cycles cannot be counted twice.

## RAM and coordinator gate

The correctness path puts scaled taps, three paired frames, a dummy and a symbol
window in the existing 65,536 B store. Payload borrows encoder scratch. Its separate
internal allocations at rate 9/10 are the existing 17,680 B S3 context and
12,420 B workspace, plus allocator/task overhead. No new task is added.

| Mode | Store used, B | Store left, B | Ring heap, B |
|---|---:|---:|---:|
| QPSK 33m333 | 59,127 | 6,409 | 0 |
| 8PSK 33m333 | 42,471 | 23,065 | 0 |
| 8PSK 26m667 | 40,642 | 24,894 | 0 |

These are derived requested sizes, not heap measurements. The closest main
layout, QPSK 9/10 26m667, measured 47,291 B Wi-Fi-up/low-water on cleaned main
(`results/2026-10-17-cleanup/runs.jsonl`). The new 9/10 allocations request the same
heap sizes; code/static growth and Wi-Fi still need `wifimem`. A second workspace
would consume another 12,420 B plus overhead. Fast fill tables will change this
layout and must be budgeted again. The legacy tight mode's floor remains 22,351 B.

Coordinator, after building/flashing the committed revision:

```
host/check.sh --moonshot --plan
PYTHON=/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 \
  RX=/path/to/leandvb host/check.sh --moonshot notes/moon-board
```

The plan prints all criteria first. Six dumps include the three target modes
(16 frames each) and three regressions; all have exact CRC/length/overlap checks
and independent decodes. Five 30 s paces include the three targets and two
regressions. Four Wi-Fi probes include all targets and tight legacy 8PSK 5m.
New modes require both free-up and low-water >=20,000 B and the 85% ceilings
above, no stalls/underruns/late fills/late switches/fatal lateness. Failed stages
do not stop later checks. The script never flashes or keys RF.

## Roll-off 0.05

DVB-S2X at exact 40 MHz support permits Rs=40e6/1.05=800/21 Msym/s,
sps=21/10, 99.747 Mb/s for normal 8PSK 9/10 with pilots. Its encoder budget is
139,822.2 cycles/frame, 118,848 at 85%; the bank budget stays 47,220.
The transition is four times sharper than alpha=0.20 in symbol units. Matching
alpha*span would suggest 48 symbols, a filter-design heuristic, not a mask proof.
Current kernels do not establish reachability.

EN 302 307-2 5.1.6/Table 2 requires alternating RO=11 with RO=10 for 0.05;
constant RO=10 still means 0.20. The local LeanDVB accepts numeric `--roll-off`
for its matched filter, but `src/leansdr/dvbs2.h` only labels BBHEADER codes
0.35/0.25/0.20/reserved and has no low-range alternation state. It therefore has
no automatic S2X roll-off signaling support. A host-only probe using a 48-symbol
0.05 pulse, alternating RO=11/10, legacy MODCOD 17 and manual `--roll-off .05`
recovered 12 of 16 contiguous frames exactly with zero corrections. This proves
manual reception of that legacy-MODCOD waveform, not general S2X support or RF.
The reproducer and raw report are in ignored `notes/moon-sys/s2x_probe.py` and
`s2x.json`. Our `host/decode.py` deliberately
accepts only 0.20 and 0.35. Neither a permissive CLI nor a 0.05 matched filter
qualifies S2X framing/MODCOD support.
[ETSI EN 302 307-2](https://www.etsi.org/deliver/etsi_EN/302300_302399/30230702/01.03.01_60/en_30230702v010301p.pdf).
