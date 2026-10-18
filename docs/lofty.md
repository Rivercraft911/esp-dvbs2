# 8PSK at 80/3 Msym/s

Round five is an offline candidate. 8PSK polls released queue pages on core 0;
core 1 makes no queue notification calls during fills. MODCODs 13/15/17 have
54/50/46 pages. The mapper reads bit planes directly, and t=10/12 BCH uses
pairs with a 4,096 B PSRAM extension. The three pinned 16-frame waveform CRCs
are unchanged. Host and Xtensa tests pass. Startup, Wi-Fi heap, fill ceilings
and keyed operation still need the coordinator's measurement. No board or
radio was accessed and no new `results/` directory was created.

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
guarded pages and workspace. MODCODs 13/15/17 use 54/50/46 pages, adding
8,448/6,336/4,224 B of internal queue heap. Resident encoder data, including
the 1,024 B mapper table, use 22,960/24,112/24,256 B. MODCODs 13/15 additionally
use 4,096 B of cached PSRAM for BCH. This is smaller than round three's
24,576 B PSRAM experiment, but its cache cost remains unmeasured.

For BCH polynomial g of degree r, T0[v] = v*x^r mod g and T1[v] = x^8*T0[v]
mod g. For the two feedback bytes u0/u1, canonical polynomial state advances as
`Rnew = (R << 16) XOR T1[u0] XOR T0[u1]`. T1's low 128 bits occupy PSRAM;
its remaining at-most-64 bits occupy padding in the resident T0 high rows.
The vector registers store canonical bytes in little-endian register order,
so the assembly shifts the byte string right. Paired tables are generated at
setup; all published payload copies and encoder stages remain timed.

Warm-up allocates no additional frame storage. Its frames and cycles are
reported separately from published encoder work. Both 8PSK halfword offsets
are exercised; resetting the mapper and descriptor histories preserves the
first payload and every frame seam. External SPI input skips synthetic warm-up.
Cache state after Wi-Fi startup and the opening ID still needs a board check.

At 240 MHz and 80 MS/s each page covers `1536*3 = 4608 cycles`. Polling
responds to the first released page, without waiting for a bank end. Shared
reads are separated by 32 NOPs, nominally 133 ns at 240 MHz, to reduce SRAM
traffic. Publication uses release/acquire sequence counters and owns each
page through rendering. Full-queue polling is reported as backpressure.
The encoder shares the command task's priority so startup/stop can run;
Wi-Fi can preempt it. Its watchdog subscription replaces core 0's idle
subscription and is restored on exit. Sleeping diagnostic policies feed it
through bounded 100 ms waits, including the opening/closing IDs.

Using a whole 16,380-sample burst conservatively, post-release coverage is
`((pages-1)*1536-16380)*3` = 195,084/176,652/158,220 cycles. This is coverage,
not average throughput or a silicon guarantee. The coordinator requires at
least 3,000 symbols of measured lead and 20,000 B of Wi-Fi minimum free RAM.

## Main regressions and admission

QPSK's sign producer kernels are identical to main's. Round four still had
50 pages, four-page bank wakes and 2,320 B of shared encoder cache, while
main used 54 pages and eight-page page wakes with the cache on heap.
Round five restores main's layout and wake policy. The fused I/Q renderer,
three warm-up frames, optional observer checks and shared producer branches
remain different. The previous 5.4k-cycle map excess has no proven silicon
cause; restored layout is a candidate fix. The QPSK ceiling stays 193,848
cycles/frame. Every historical 2% ceiling remains pinned.

An 8PSK frame lasts `22194*3*240e6/80e6 = 199746 cycles`; 85% is 169,784.
The bank deadline is `15740*3 = 47220 cycles`, with a 40,137-cycle ceiling.
Frame sizes follow EN 302 307-1 clause 5.5; bank geometry is in `radio_s3.h`.
Trace replay excludes cache/bus stalls and interrupt delays. It establishes
neither silicon timing nor admission; the full 15% goal remains open.

The user's keyed estimate scales 46,000 cycles by `199746/239630` to about
38,344 cycles/frame. Subtracting that from the frame and its 85% ceiling gives
161,402 and 131,440 cycles/frame, respectively. The coordinator uses the
requested rounded targets: 160k to hold and 132k for 15% keyed reserve.
These are planning estimates, not keyed measurements of this candidate.

After the coordinator builds/flashes the final revision with N0CALL:

```sh
caffeinate -i python3 host/jitter.py notes/eight-lofty-round5
```

Each mode gets a clean 20 s run, a 5 s observed default-policy run and a 5 s
observed comparison. 8PSK compares polling with page wakes; QPSK compares
page with bank wakes. Including IDs, collection takes about one minute per
mode. `--plan` opens no device. Faults are retained and later modes continue.
The clean gate requires ESP_OK, completed playback, zero queue/deadline
faults, observer off, fill <=40,137 cycles, lead >=3,000 symbols, Wi-Fi minimum
>=20,000 B, encoder stack >=512 B, mean <=160,000 cycles/frame for 8PSK or
<=193,848 for QPSK. The separate 132k reserve target remains visible.

Executed-instruction replay predicts 151.7k/156.5k/135.8k cycles/frame for
13/15/17, including a 4,823-cycle payload allowance. It excludes cache/bus
stalls, task preemption and live publication overhead. It cannot establish
any silicon ceiling. The 132k goal, startup of 17 and keyed qualification
remain open until measured.
