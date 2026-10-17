# 8PSK at 80/3 Msym/s

The candidate keeps the encoder's hot GCS tables in internal RAM. All three
16-frame waveforms retain their CRCs and decode without corrections. Board
admission is pending. The coordinator's `3cbf37c` image decoded correctly but
starved its queue under flash-cache pressure. This round accessed no board or
radio. [Records and limits](../results/2026-10-17-lofty-ram/README.md).

## Tables and framing

The five-symbol inverse-hold root-raised-cosine pulse is unchanged: roll-off
0.35, three samples/symbol, Kaiser beta 1.5. `host/lofty.py` derives the taps,
part rounding, waveform and rounded-table spectrum independently. Its ideal
DAC-hold mask margins remain +0.250 dB upper, +0.225 dB lower and +6.970 dB
far. The radio's analog response still needs measurement.

Four output symbols use the eight phases [-4..3], split into [-4..-2],
[-1..1], [2..3]. Preserve all three independent roundings. The first part's
last eight samples depend on only two phases; its first four need three.
The newest part's duplicated rows are removed. Storage is now
`512*4*4 + 64*8*4 + 512*12*4 + 2*64*4*4 = 36864 B`, saving 16,384 B.
Keeping the last two vectors adjacent spends 896 B to save two renderer
instructions per twelve samples. Offset-binary biases still sum to 512 per
axis; packed additions followed by XOR `0x80200` produce the same DAC words.

For chronological eight-phase history `h`, publication uses:

```
A = (h >> 15) & 511; B = (h >> 6) & 511; C = h & 63
descriptor = (A << 4) | (3*B << 13) | (C << 24)
```

PIE vector loads ignore the low four address bits (ESP32-S3 TRM v1.8,
1.8.88). The renderer costs 24 model cycles/twelve samples, versus 22 before.
Only a board run can establish the changed SRAM contention and bank maximum.

The encoder maps four phases per halfword into existing context scratch.
It publishes a first page before expanding the rest of the PL frame. This
keeps bulk framing out of the longest gap before publication. A 720 B run
buffer and 320 B first-page buffer reuse idle LDPC scratch. Queue publication
borrows those words directly. There is no intermediate page copy. Partial
phase groups and descriptor history survive frame boundaries.

## RAM and coverage

The fixed store holds `36864 + 38*528 + 8144 = 65072 B`: pulse tables,
guarded pages and workspace. MODCODs 13/15/17 use 48/49/44 pages, with
5,280/5,808/3,168 B of additional queue heap. Complete resident encoder data,
including the mapper, use 23,024/24,176/24,320 B. PRBS storage is sized to the
active BB frame; BCH, Gold, LDPC program/columns and header CRC stay resident.
All stage work and input copies remain timed. Only full-queue waits are excluded.

At 240 MHz and 80 MS/s, each page covers `1536*3 = 4608 cycles`. After the
two-page wake threshold and a complete 16,380-sample bank burst, coverage is
`((pages-2)*1536-16380)*3`: 162,828/167,436/144,396 cycles. The gate compares
this against the measured publication gap plus wake cost. More queue coverage
does not establish average encoder capacity.

The main merge places allocator code in flash. Linked heap space improves,
but Wi-Fi allocation and fragmentation still need the coordinator's probe.
The record estimates internal minima from the previous silicon run and actual
allocation/linker deltas. No host heap counter is used as a measurement.

## Main regressions and admission

Synthetic 8PSK 9/10 at 5m uses paired byte BCH instead of register-nibble BCH.
QPSK uses fused load/add/store for disjoint I/Q fields, and vector prefix
updates for sign histories. Its 50-page/four-page wake geometry preserves
`50-4 = 54-8` pages of post-wake coverage and shares 2,320 B of resident
encoder data with the store. Every original 2% ceiling and heap floor remains
pinned in `tests/data/lofty_gates.json`.

An 8PSK frame lasts `22194*3*240e6/80e6 = 199746 cycles`; 85% is 169,784.
The bank deadline is `15740*3 = 47220 cycles`, with a 40,137-cycle ceiling.
Frame sizes follow EN 302 307-1 clause 5.5; bank geometry is in `radio_s3.h`.
Trace replay excludes cache/bus stalls and interrupt delays. It establishes
neither silicon timing nor admission; the full 15% goal remains open.

After the coordinator builds/flashes the committed revision with N0CALL:

```sh
PYTHON=/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 \
RX=/Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb \
  host/check.sh --lofty notes/eight-lofty-round2-board
```

`--plan` opens no device. The slim runner keeps going, runs main first,
then dumps/decodes all three candidates and runs 20/20/10 s Wi-Fi-up paces.
Both cores must retain 15%, free/minimum heap must reach 20,000 B, stack
must retain 512 B, and queue/overlap/deadline checks must pass. It never
flashes, writes NVS or transmits. Keyed qualification remains separate.
