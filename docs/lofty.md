# 8PSK at 80/3 Msym/s

Round seven restores round four's sleeping producer at priority 24 and
bank-end wakes for 8PSK. Polling, controller priority changes and the core-0
watchdog substitution are removed. The bit-plane mapper, split BCH pairs
and 54/50/46 pages for MODCODs 13/15/17 remain. No more pages are allocated.
The first page now precedes bulk mapping as well as bulk PL framing.
This is an offline candidate: 17 startup, sustained lead and silicon timing
still need the coordinator. No board or radio was accessed.

## Tables and framing

The five-symbol inverse-hold root-raised-cosine pulse is unchanged: roll-off
0.35, three samples/symbol, Kaiser beta 1.5. `host/lofty.py` independently
derives the taps, rounding and waveform. Rounded-table ideal DAC-hold mask
margins remain +0.250 dB upper, +0.225 dB lower and +6.970 dB far. The radio's
analog response still needs measurement.

Four chronological phases use three separately rounded pulse contributions.
The oldest part's last eight samples depend on two phases; its first four
need three. Storage is
`512*4*4 + 64*8*4 + 512*12*4 + 2*64*4*4 = 36864 B`.
Biases sum to 512 per axis; packed addition and XOR `0x80200` retain the DAC
words. For chronological eight-phase history h, publication uses:

```
A = (h >> 15) & 511; B = (h >> 6) & 511; C = h & 63
descriptor = (A << 4) | (3*B << 13) | (C << 24)
```

PIE vector loads ignore the low four address bits (ESP32-S3 TRM v1.8,
1.8.88). The renderer costs 24 model cycles/twelve samples. This instruction
model excludes cache, bus and interrupt delays.

The encoder maps four phases per halfword into existing context scratch.
It first maps 512 data symbols and the final 96 symbols. The tail preserves
inter-frame history. The first queue page uses the header and mapped head;
the next read maps the middle and expands PL framing backwards. The input
workspace must remain unchanged until the last read. A 720 B run buffer and
320 B first-page buffer reuse idle LDPC scratch. The extra input pointer fits
pre-vector padding; target scratch stays at main's offset of 608 B.

## Handoff and RAM

The fixed store holds `36864 + 38*528 + 8144 = 65072 B`: pulse tables,
guarded pages and workspace. MODCODs 13/15/17 use 54/50/46 pages, adding
8,448/6,336/4,224 B of internal queue heap. Resident encoder data, including
the 1,024 B mapper table, use 22,960/24,112/24,256 B. MODCODs 13/15 also use
4,096 B of cached PSRAM for BCH.

For BCH polynomial g of degree r, T0[v] = v*x^r mod g and T1[v] = x^8*T0[v]
mod g. With feedback bytes u0/u1,
`Rnew = (R << 16) XOR T1[u0] XOR T0[u1]`.
T1's low 128 bits occupy PSRAM; the remaining at-most-64 bits use padding in
the resident T0 high rows. Tables are generated at setup. Payload copies and
all encoder stages remain timed.

A full producer arms its notification then rechecks the release/acquire
queue counters before blocking indefinitely. The consumer never waits. It
retains each page through rendering and wakes at most once per completed
physical bank, after saving overlap. A split bank's first callback cannot
notify. Diagnostic page wakes remain available. Three synthetic warm-up
frames use no extra frame buffer; seed and histories reset before publication.

At 240 MHz and 80 MS/s a page covers `1536*3 = 4608 cycles`. With a two-page
wake threshold and a conservative whole-bank burst, available coverage is
`((pages-2)*1536-16380)*3 = 190476/172044/153612 cycles`.
This is queue geometry, not a throughput or minimum-lead guarantee. Moving
bulk mapping after the first page reduces the modeled no-publication gap by
16,960 cycles, or `16960/9 = 1884` symbols at nine CPU cycles/symbol.
Mode 17 retains two more pages than round four, another 1,024 symbols.
Only silicon can confirm the required 3,000-symbol lead.

## Evidence and collection

Native waveforms retain CRCs 2061900610/1873478180/233370528 for the three
16-frame 8PSK streams. All eight QPSK waveform CRCs and the 200-frame bank-wrap
test also pass. The actual handoff stress checks 24,576 pages, partial-page
ownership, physical and 32-bit sequence wraps, notification before blocking
and stopping a full producer. It has host pacing and makes no deadline claim.
Xtensa tests check guarded slices, frame seams, C/PIE mapping and ISR teardown.

Instruction replay predicts mean work of 151.9k/156.7k/135.9k cycles/frame
for 13/15/17, including a 4,823-cycle payload allowance. Mode 17's modeled
first-publication gap is 77,246 cycles, versus 94,206 before this change.
Replay uses executed instructions, never QEMU CCOUNT, and excludes cache/bus
stalls, preemption and live publication overhead. The 132k goal remains open.

The core-0 source audit finds yielding prefill and blocking playback/teardown
in the command task. It does not identify the silicon preemptor. The previous
host-injected peer pause established a possible mechanism only; it did not
justify retaining polling after two failed silicon rounds.

QPSK retains round six's common context offsets and removal of the extra
inner-loop PIE-flag check. Its default remains priority-24 page wakes. The
coordinator measured 192,820 cycles/frame and a 39,806-cycle bank on 65dada4.
No code from the unmerged QPSK experiment was borrowed.

Frame duration is `22194*3*240e6/80e6 = 199746 cycles` (EN 302 307-1, 5.5).
The supplied keyed estimate is `46000*199746/239630 = 38344 cycles/frame`.
This leaves 161,402 cycles to hold or 131,440 cycles for 15% reserve. Collection
uses the requested rounded 160k/132k targets. These are estimates, not keyed
measurements. Bank geometry in `radio_s3.h` gives `15740*3 = 47220 cycles`
per bank and a 40,137-cycle fill ceiling.

After the coordinator builds/flashes the final revision with N0CALL:

```sh
caffeinate -i python3 host/jitter.py notes/eight-lofty-round7
```

Each mode gets 20 s clean, 5 s observed default and 5 s observed comparison,
about one minute including IDs. 8PSK compares bank/page wakes; QPSK compares
page/bank wakes. `--plan` opens no device. Failures are retained and collection
continues. Clean admission still requires ESP_OK, full playback, zero queue
or deadline faults, fill <=40,137 cycles, lead >=3,000 symbols, Wi-Fi minimum
>=20,000 B, stack >=512 B, and mean <=160k for 8PSK or <=193,848 for QPSK.
The separate 132k target remains visible. No silicon gate is relaxed.
