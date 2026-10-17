# 8PSK at 80/3 Msym/s

`26m667` now accepts 8PSK end to end. This is an offline candidate, not a
measured operating point. The 9/10 trace model fits 15% reserve. The 2/3 and
5/6 models meet the frame deadline but fail that reserve. No board or radio
was accessed. [Evidence](../results/2026-10-17-eight-lofty/README.md).

## Waveform

The adopted candidate uses a five-symbol inverse-hold root-raised-cosine
pulse, roll-off 0.35, three samples/symbol, Kaiser beta 1.5. Shortening to
three or four symbols failed the Annex A mask in the pulse model. Five
symbols pass. `host/lofty.py` independently derives the integer taps and
checks the exact covariance of the rounded tables, including the DAC hold.
At peak 128 its margins are +0.250 dB upper, +0.225 dB lower and +6.970 dB
beyond the last mask point. The analog radio response is unknown.

Four output symbols occupy 12 samples. Their eight contributing symbols are
[-4..3], split into [-4..-2], [-1..1], [2..3]. Each part is rounded once per
axis in DAC LSB, using 2^-30 constellation coordinates and taps in 1/256 LSB.
Offset-binary biases sum to 512 per axis. Adding the three packed words and
XORing 0x80200 gives signed I/Q without cross-field carry. The newest pair
contributes zero to the first vector. Its other two vectors are duplicated
to absorb one otherwise unwanted index bit. Storage is
`2*512*12*4 + 2*128*4*4 = 53248 B`.

This avoids the large full-axis tables: five values across eight symbol
positions would require 5^8 possibilities per axis. A 22.5-degree rotation
reduces the alphabet to four values, but a six-symbol pulse still needs up
to seven symbols per vector. Splitting weighted sign streams then needs
four vector lookups instead of QPSK's two. Those estimates did not justify
keeping another kernel. The retained solution instead folds address work
into the producer and uses the shorter mask-compliant pulse.

## Queue and cores

For chronological packed phases `h` in the last eight symbols, the producer
publishes one descriptor per four symbols:

```
A = (h >> 15) & 511; B = (h >> 6) & 511; C = h & 63
descriptor = (3*A << 4) | (3*B << 15) | (C << 26)
```

The renderer extracts offsets at bits 0, 11 and 21. PIE vector loads ignore
the low four address bits (ESP32-S3 TRM v1.8, 1.8.88). The C offset includes
one B bit, handled by the duplicated rows. There is no bank preparation or
multiply on core 1. Eight vector loads, fused additions and three stores
render twelve samples in 22 model cycles, 1.833 cycles/sample.

The new core-0 loops use cached flash instructions to preserve legacy heap
floors; all core-1 fill code stays in IRAM. Instruction-cache effects still need
a board measurement. Core 0 maps FEC bit planes through 256-entry and 16-entry tables. Their
outputs OR together, so the mapping cache is 1,088 B. Header and pilot words are
prepared once. A PIE pass publishes scaled histories, preserving partial
groups and history across frame boundaries. All preparation remains timed
inside MAP. Only full-queue waits are excluded.

There are 36 guarded 528 B pages, 128 groups and 1,536 samples per page.
Twenty-three pages share the fixed 65,536 B store; thirteen use a 6,864 B
internal allocation. The fast workspace uses 8,144 B separately. The static
store and encoder stack sizes stay fixed. Queue coverage is 165,888 cycles. After the
two-page wake threshold and one complete bank burst, 107,532 cycles remain
for publication gaps and wake allowance. Counters use acquire/release;
physical cursors survive sequence wrap.

## Limits and coordinator gate

At 240 MHz and 80 MS/s, a 15,740-sample bank lasts 47,220 cycles. Its 85%
ceiling is 40,137. A normal pilot-bearing 8PSK frame has 22,194 symbols
(EN 302 307-1, 5.5/Table 11), hence `22194*3*3 = 199746 cycles/frame` and a
169,784-cycle ceiling. Payload bit rate is `(Kbch-80)*(80e6/3)/22194`:
51.618, 64.594 and 69.823 Mb/s for 2/3, 5/6 and 9/10.

The trace model includes all encoder/framing/preparation instructions and a
4,823-cycle input-copy planning floor from the main SPI measurement. It
excludes silicon cache/bus stalls and Wi-Fi interrupts. A 25% inflation
sensitivity fails even at 9/10. The fill prediction transfers main QPSK's
measured residual and adds 1,000 cycles for the changed wrapper. Neither is
a hardware bound. In particular, 2/3 and 5/6 are not reserve-qualified;
no slower fallback above 20 Msym/s has been established for those rates.

After the coordinator builds/flashes this revision with N0CALL stored:

```sh
host/check.sh --lofty --plan
PYTHON=/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 \
RX=/Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb \
  host/check.sh --lofty notes/eight-lofty-board
```

The runner prints criteria first, keeps going, runs 10 s main regressions
with unchanged ceilings/floors before the candidate, then 16-frame dumps,
LeanDVB, 20/20/10 s Wi-Fi-up paces and heap probes. Both cores must retain
15%, heap up/min must reach 20,000 B, stack must retain 512 B, and every
queue/overlap/deadline check must pass. An overall admission failure is
expected if the lower code rates retain the modelled costs. It never
flashes, writes NVS or transmits. Keyed qualification is still separate.
