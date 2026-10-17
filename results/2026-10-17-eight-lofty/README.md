# 8PSK 26m667, offline

`26m667` now accepts 8PSK, and all three target waveforms decode bit-exact.
Only 9/10 clears the offline 15% capacity model. The full 2/3 and 5/6
reserve goal remains open. There is no measured or keyed result here, and
no slower fallback above 20 Msym/s has been qualified for those code rates.
No board, serial port or radio was accessed.

Source `49da54e`, base `88f5a64`, ESP-IDF v6.1 / Xtensa GCC 15.2.0.
QEMU supplies executed instructions, not timing. Replay uses the LX7/PIE
single-issue/load-use model (TRM v1.8, 1.7/Table 1.7-2), per-lane gathers,
taken redirects and an 11-cycle division allowance. Cache/bus stalls and
real Wi-Fi interrupts are excluded. The new core-0 loops use cached flash
instructions; core 1 stays in IRAM.

| 8PSK, 80/3 Msym/s | 2/3 | 5/6 | 9/10 |
|---|---:|---:|---:|
| Encoder + input planning floor, cycles/frame | 180,821 | 186,305 | 162,210 |
| Core 0, including 1,000 cycles/bank publication/wake allowance | 43,747 | 45,043 | 39,347 |
| Estimated free internal RAM with Wi-Fi, B | 26,637 | 23,943 | 30,439 |
| 15% encoder reserve model | fail | fail | pass |
| LeanDVB contiguous bit-exact frames recovered / supplied | 14 / 16 | 14 / 16 | 14 / 16 |

A frame lasts 199,746 cycles. Its 85% ceiling is 169,784; each core's bank
ceiling is 40,137 of 47,220 cycles. Input adds the measured main SPI-copy
mean of 4,823 cycles as a planning floor, not a bound or integrated input
measurement. A 25% kernel inflation fails even at 9/10.

The five-symbol .35 pulse uses 53,248 B of tables. The mapper publishes
scaled offsets into 36 guarded pages. The two bit-plane mapping tables
need 1,088 B. The assembled render loop costs 22 cycles per twelve samples,
1.833 cycles/sample. Its full-bank prediction is **38,224 cycles**: ideal
fresh-sample issue cost plus main QPSK's 8,360-cycle measured residual and
1,000 additional wrapper cycles. That transfer is an estimate, not a bound.
The exact rounded-table/ideal-hold mask has positive margins: +0.250 dB
upper, +0.225 dB lower, +6.970 dB far. RF response remains unknown.

Heap estimates use the measured main QPSK Wi-Fi free value 21,207 B,
replace its 53,952 B of requested allocations with this mode's requests,
subtract the linked 528 B heap-start increase and reserve another 64 B for
allocation alignment. Only the board can confirm free/largest/minimum heap.

All 58 retained waveform CRCs and 140 FEC phase CRCs pass. New 16-frame CRCs
are 2,061,900,610 (13), 1,873,478,180 (15), 233,370,528 (17). Every C sample
matches independent Python; LeanDVB reports zero payload errors, LDPC
checks/flips and BCH corrections/failures. QEMU checks 4,320 guarded slices,
288 preparation alignments and 36 mapper frames across all code rates and
pilot settings. A 200-frame native stream crosses the complete physical
bank-wrap period. Allocation-failure, repeated-command, sequence-wrap and
retained fractional-QPSK checks pass. Firmware builds. Native clocks and
heap counters are synthetic and supply no timing or memory evidence.

The raw compact records and artifact SHA-256s are in `runs.jsonl`. Large
traces, ELF files and dumps stay in ignored `notes/`. Reproduction commands
are in [the harness](../../tests/lofty/README.md); the design and equations
are in [lofty.md](../../docs/lofty.md).

After the coordinator builds/flashes the committed revision with N0CALL:

```sh
host/check.sh --lofty --plan
PYTHON=/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 \
RX=/Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb \
  host/check.sh --lofty notes/eight-lofty-board
```

Criteria print before commands. Main runs first, including QPSK 33m333,
with unchanged timing ceilings and heap floors. Candidate checks continue
after failures and retain the full 15% requirement for all three code rates.
The 20/20/10 s paces keep Wi-Fi RX up; heap up/min must reach 20,000 B,
stack 512 B, and no queue/overlap/deadline error is allowed. The runner never
flashes, writes NVS or transmits. Overall admission is expected to fail if
2/3 and 5/6 retain their modelled costs. Keyed qualification is separate.
