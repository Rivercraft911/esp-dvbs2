# Hold correction and the 80 MS/s budget

**Host models and QEMU, no new board measurement.** The 4-sps table improves to 39.46 dB MER
at roll-off 0.20 and 45.97 dB at 0.35, with positive mask margins. A staged-row consumer fits
the modelled bank deadline, but its producer work prevents a complete 8PSK 5/6 solution.
The 5-sps prototype misses the deadline. No board, SDR, flashing or RF access occurred.

Source commit `1268b94` preserves the tested code. The sandbox blocked this worktree's Git
index, so commits are held in local `notes/shaper-next.bundle`; source hashes in `runs.jsonl`
identify the exact files. Firmware and standalone QEMU harness build cleanly with ESP-IDF
v6.1. All 18 host test scripts pass; QEMU passes 324 word comparisons, guards and independent
Python checksums. `firmware/main/dvbtx.c` and the radio driver are unchanged.

The pulse divides the ideal RRC frequency response by `sinc(f/Fs)` before inverse-transform,
Kaiser beta 1 windowing, truncation and worst-case branch scaling to 508 LSB. This changes
only table values. [The model](../../host/shaper.py) tests 10-bit bounds exhaustively; corrected
nine-symbol rounded peaks are 507..508 LSB per branch. Standard: EN 302 307-1 V1.4.1 clause
5.6 p35; Annex A pp37-38 explicitly specifies x/sin(x) correction for rectangular inputs.

All following quality figures are modelled, with the ideal hold and receiver, 16,384 cyclic
symbols, seed 7. Mask columns are upper/lower/far margins in dB. The far -40 dB extension from
point S through Fs/2 is a design assumption. No analog filter, RF noise or DAC nonlinearity.

| Profile | MER before / after, dB | Mask before, dB | Mask after, dB |
|---|---:|---:|---:|
| 8psk-n4-a20 | 35.67 / 39.46 | +0.250/+0.201/+2.684 | +0.209/+0.240/+2.474 |
| 8psk-n4-a35 | 38.75 / 45.97 | +0.228/+0.250/+7.666 | +0.155/+0.250/+7.323 |
| 8psk-n5-a20 | 37.09 / 39.54 | +0.250/+0.244/+2.268 | +0.205/+0.244/+2.132 |
| 8psk-n5-a35 | 40.75 / 46.06 | +0.206/+0.250/+7.521 | +0.156/+0.250/+7.296 |
| 8psk-n8-a20 | 38.67 / 39.69 | +0.234/+0.247/+1.851 | +0.202/+0.247/+1.792 |
| 8psk-n8-a35 | 44.14 / 46.05 | +0.176/+0.250/+7.330 | +0.156/+0.250/+7.238 |
| qpsk-n40-a20 | 39.63 / 39.69 | +0.201/+0.249/+1.570 | +0.199/+0.249/+1.567 |
| qpsk-n40-a35 | 46.56 / 46.65 | +0.157/+0.250/+7.170 | +0.156/+0.250/+7.168 |

At n4, pulse-only MER for 9/11/13 symbols is 39.37/45.42/47.13 dB (.20),
and 46.28/51.39/54.97 dB (.35). Corresponding table MER is 39.46/45.25/46.76 dB
and 45.97/50.42/53.19 dB. The full JSON lines include every profile and floating pulses.

Table RAM for 9/11/13 symbols: n4 32/32/64 KiB; n5 64/64/128 KiB; n8 64/64/128 KiB;
n40 32/64/64 KiB. Longer spans exist only in the host model. An extra part costs at least
0.25 cycles/sample; at n4 the optimistic 11-symbol steady-loop floor plus 1000 control cycles
is already 42.63k cycles/bank before extra prologue/wrap work. Thirteen symbols need at
least 46.73k. Neither has a robust 80 MS/s cycle budget; n8 with 13 symbols also exceeds
the existing 64 KiB table store.

Deadline at 80 MS/s: `(16380 - 64 - 2048) * 240/80 = 42804 cycles = 178.35 us`.
Geometry: [radio_s3.h:8](../../firmware/components/radio_s3/include/radio_s3.h#L8).
[The cycle model](../../host/shaper_cycles.py) replays assembled instructions using TRM v1.8
1.7.1 pp65-66 and Table 1.7-2. It calibrates n8 29065 cycles to the prior measured
29088 ([shaper results](../2026-10-16-shaper/README.md)). Estimates include an **assumed**
1000-cycle wrapper/control allowance; n4 includes worst wrapped-bank PIE tails. The +22%
column is a sensitivity test from the encoder model error, not a shaper error bound.

| Fill estimate | Cycles/bank | Slack | Slack with +22% kernel |
|---|---:|---:|---:|
| n4 PSRAM copy on fill core | 55726 | -12922 | -21303 |
| n4 producer-staged bytes | 39095 | +3709 | -4672 |
| n4 internal frame source | 39095 | +3709 | -4672 |
| n4 producer-staged row pointers | 26602 | +16202 | +10569 |
| n5 internal bytes, existing geometry (lower bound) | 69629 | -26825 | -41924 |
| n5 aligned geometry, internal bytes | 69629 | -26789 | -41888 |

Raw benchmark predictions: n4 37,576 cycles; staged n4 25,275; row preparation 32,814;
n5 68,575. The old fill with its measured overhead estimates about 67.4k cycles. The lean
copy estimate assumes the prior 4.053 cycles/B small-copy rate
([source](../2026-10-16-shaper/runs.jsonl)). The n5 alignment proposal uses lead60/tail2040,
stride14280 and deadline42840; four-symbol groups and every wrap then align to 128 bits.
That geometry is host-tested only. Existing radio scheduling still uses 40 MS/s.

Double-buffered n4 byte staging costs 8206 B, row staging 32816 B. Row preparation costs
at least 160 M cycles/s at 20 Msym/s and makes the combined CPU budget worse. Direct internal
PLFRAME slots avoid per-bank copies: three slots plus history cost 66606 B. This exceeds the
prior 57-58k free internal heap; sharing n4's unused 32 KiB table half could make RAM plausible.
The n5 table uses that whole store. Two frames plus about 3.3k retained tail need about 47.7k B,
with a tested retirement queue still required. Total-size arithmetic is not an allocation test.

[Measured 5/6 encoder maxima](../2026-10-16-fec-limit/README.md#cycles-per-frame-pie-path)
are about 313k cycles GCS and 367k TS. Available cycles/frame are 332910 at 16 Msym/s and
266328 at 20. GCS leaves only 19910 cycles at 16 and misses by 46672 at 20; TS misses both.
At16, n5 needs <=2.554 cycles/sample plus internal-frame input and near-zero ingress overhead.
At20, GCS needs at least a 14.9% encoder reduction before ingress, and the n4 byte fill has
little nominal margin. Keeping the separate row pass needs about 2.46 cores total at 20.
None of these prototypes demonstrates complete 16 or 20 Msym/s 5/6 operation.

Reproduce with [the harness instructions](../../tests/shaper_pie/README.md), then
`python3 host/shaper_budget.py --disassembly notes/shaper.dis --qemu notes/qemu.log`.
The long local derivation, memory alternatives and all expected hashes are in
`notes/shaper-next.md`. No extra public research report is committed.

Coordinator, once board use resumes:

```sh
source ~/.espressif/tools/activate_idf_v6.1.sh
IDF_COMPONENT_MANAGER=0 idf.py -C firmware build flash
/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 host/board.py \
  membench --reset --timeout 120 --out results/<new-date>-shaper-board/runs.jsonl
/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 host/shaper.py \
  verify results/<new-date>-shaper-board/runs.jsonl
```

Pass: all 30 expected checks match; n4 `_all`, `_rows`, `_rows_all` equal its C/PIE output.
Expected new output checks: shape4 **3253905476**, shape4a35 **3947536958**, shape5
**2783243692**, shape5a35 **188012256**. All build and legacy-profile checks are retained in
the `membench_expected` JSON record. Stage timing records have check 0 since addresses vary.
Retain timing minima/medians/maxima. Any kernel >22% above the raw model invalidates that
stress allowance. Future integrated 80 MS/s checks must show whole-bank cycles <42804,
zero overlap mismatches, no late/fatal fills or underruns and bit-exact decoded payload.
The present QPSK/40 MS/s `dvbpace` cannot qualify the new queues or 80 MS/s. DAC contention,
analog response, allocations and RF quality remain unverified.
