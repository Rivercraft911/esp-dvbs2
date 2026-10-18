# 8PSK at 80/3 Msym/s

The 8PSK queue admits complete data PLFRAMEs. If the next frame is not
published at a boundary, playback inserts standard dummy PLFRAMEs and
rechecks. It never waits for encoder CPU time. The 54/50/46-page queues and
qualified QPSK 33m333 encoder, renderer and queue geometry are retained.
This is an offline candidate; the coordinator must measure fill time, dummy
counts and Wi-Fi RAM. No board or radio was accessed.

## Complete-frame admission

A data frame is 22,194 symbols at three samples/symbol:
`22194*3 = 66582 samples`, or `66582*3 = 199746 CPU cycles` at 240 MHz.
The coordinator's 593f48f run had a 237,735-cycle cold live maximum on 15.
An admission rule that assumes the missing data will arrive within one frame
cannot cover that observation. The new rule requires the entire frame.

A page holds 128 four-symbol descriptors, normally 1,536 samples. Frames
alternate between zero- and two-symbol group prefixes. The producer publishes:

- zero prefix: 43 full pages and a 534-sample tail;
- two-symbol prefix: a 1,530-sample first page, 42 full pages and a
  540-sample tail.

Both need 44 pages. The tail is published immediately, including its last
two symbols when necessary. The next frame skips the repeated two-symbol
prefix. EOF no longer depends on the following frame's FEC work. Page bounds
and cumulative sample ends use existing guard padding; the first prefetch
guard remains zero. Page size stays 528 B.

The producer release-publishes the frame counter after its final page.
The consumer acquire-loads that counter only at PLFRAME boundaries. Once
admitted, every sample is already owned by the consumer. At a dummy decision
there are no older data pages left; because every queue has at least 44
slots, the producer can finish the next frame without a consumer release.
Dummy admission also wakes a producer whose full-queue wait became obsolete
while the previous frame played. Ordinary releases retain bank-end wakes.
No per-page kernel calls are added to the default policy.

## Dummy waveform and accounting

[EN 302 307-1 V1.4.1, clauses 5.5.1 through 5.5.4](https://www.etsi.org/deliver/etsi_en/302300_302399/30230701/01.04.01_60/en_30230701v010401p.pdf)
defines MODCOD 0, the 90-symbol header, 36 pilot-less slots and PL scrambling.
Each dummy is `90+36*90 = 3330 symbols`, or 9,990 samples here. No data frame
or payload byte is discarded or repeated when a dummy is selected.

The renderer rounds three pulse contributions per four-symbol group.
`3330 mod 4 = 2`, while `6660 mod 4 = 0`. Playback rechecks after each dummy
but completes a pair before admitting data. This preserves the original
four-symbol rounding and aligned vector stores. Every dummy is counted;
`dummy_alignment` is the subset completing pairs. Two prepared pair views
cover both boundary prefixes, using 13,376 B of PSRAM and no extra queue pages.
Only the first one or two descriptors at a splice need repaired filter
history; the queued data descriptors remain unchanged.

`dummy_startup` counts selected dummies before three data frames have finished.
`dummy_steady` counts later choices; its target is zero. Counts include a
selected final dummy if playback stops partway through it. `frames_played`
counts complete data frames; `positions` also includes dummy choices. The
conservative full-frame rule and small queues can insert steady dummies even
when average encoder work fits. The coordinator reports those counts rather
than hiding them in an encoder mean.

## Tables, rendering and RAM

The five-symbol inverse-hold RRC pulse is unchanged: roll-off 0.35, three
samples/symbol, Kaiser beta 1.5. `host/lofty.py` independently derives its
roundings. Rounded-table ideal DAC-hold mask margins remain +0.250 dB upper,
+0.225 dB lower and +6.970 dB far. Analog response still needs measurement.

The separately rounded pulse parts occupy
`512*4*4 + 64*8*4 + 512*12*4 + 2*64*4*4 = 36864 B`.
For chronological eight-phase history h:

```
A = (h >> 15) & 511; B = (h >> 6) & 511; C = h & 63
descriptor = (A << 4) | (3*B << 13) | (C << 24)
```

Four renderer words now share one source-pointer advance. The loop takes
93 model cycles per 48 samples, or 23.25 cycles/twelve samples, versus 24.
Calls, tails, cache/bus contention and interrupts are additional. This saves
about 984 issue cycles over a 15,740-sample bank, before changed wrapper cost.
It does not establish the 40,137-cycle silicon ceiling.

The fixed store remains `36864 + 38*528 + 8144 = 65072 B`. Queue heap remains
8,448/6,336/4,224 B for 13/15/17. Resident encoder data remains
22,960/24,112/24,256 B, with the existing 4,096 B PSRAM BCH extension on 13/15.
The target context scratch stays at offset 608 B. The new mux control stays
in flash to limit internal RAM growth. Heap placement must still be checked
against the 20,000 B Wi-Fi minimum on silicon.

## Checks and collection

No-dummy 16-frame CRCs remain 2061900610/1873478180/233370528. All eight QPSK
CRCs and its 200-frame bank-wrap test remain unchanged. Forced dumps insert
six dummies before data frames 0, 1 and 5, including both boundary prefixes.
Every DAC word matches the independent Python encoder/convolution. LeanDVB
recovers 13/12/12 contiguous data frames for 13/15/17 with zero payload bit
errors, BCH corrections or LDPC flips. The stress test pauses the producer
while the consumer plays dummies, then checks 24,576 pages across physical
and sequence wraps. These host checks do not establish hardware deadlines.
Xtensa checks retain guarded slices, mapper frames and ISR teardown.

After the coordinator builds/flashes the final revision with N0CALL:

```sh
caffeinate -i python3 host/jitter.py notes/eight-lofty-round8
```

Each mode gets 20 s clean, 5 s observed default and 5 s observed comparison,
about one minute including IDs. 8PSK compares bank/page wakes; QPSK compares
page/bank wakes. The runner never flashes or transmits; `--plan` opens no port.

The 8PSK clean gate requires ESP_OK, complete playback, zero queue faults and
late fills/switches, fill <=40,137 cycles, Wi-Fi minimum >=20,000 B and stack
>=512 B. Startup dummies are allowed; steady dummies are reported with target
zero. Encoder means and minimum lead remain diagnostics. The qualified QPSK
mean/lead gates remain unchanged. All raw failures are retained.
