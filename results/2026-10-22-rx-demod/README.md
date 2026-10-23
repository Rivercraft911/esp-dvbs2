# Receiver demodulator on real dumps, the board and the twin

The demodulator (`firmware/components/dvbs2rx/demod.c`, with PIE kernels in `demod_pie.S`)
turns the S3 receiver's own 16 MS/s dump into hard bits that equal the transmitted FEC
codeword in every complete frame of both real short-frame captures: 22 of 22 at QPSK 3/4 and
21 of 21 at QPSK 8/9. Over the normal-frame corpus it gets 98 of 100 frames exact at receiver
gains up to 48 (the ADC saturates from 56); each of the two misses (6 and 7 bit errors) comes
from a 180-symbol burst in the received signal that a float reference receiver and LeanDVB's
LDPC also see.

On the board, with the radio off, the demodulator processes a 12,288-word unit (768 µs of
signal, 184,320 cycles at 240 MHz) in at most 127,497 cycles and 79,369 on average, down from
418,800 and 178,450 before the PIE kernels. Every frame of the replay is exact. Acquisition
costs at most 74,772 cycles per unit live on receiver noise, down from 238k, and at most 73k in
replays of noise, a CW tone and dropped units.

On the digital twin, 30 s of QPSK 3/4 short frames at 18 dB give 3,583 of 3,583 decoded frames
exact (the first frame is used to lock) and an implementation loss of 0.22 dB against a float
reference receiver.

The demodulator measures 26 to 31 dB pilot MER on most unclipped captures and 21 to 26 dB on
the two weakest; LeanDVB reports 7 to 16 dB on the same samples. The transmitter's carrier
leak sits at about -17 dB in its own frame and limits any receiver that does not remove it;
this demodulator removes it after derotation.

## Setup

- Input: the dump's int8 I and Q, floor(s10 / 4), at 16 MS/s, exactly what `rx_unpack` makes.
  Captures came from an S3 receiver (firmware `s3_receive`) fed by an S3 transmitter through coax
  and 30 dB of attenuation, 1 Msym/s, roll-off 0.35, pilots, station KJ5SCH payload model.
  Short frames and the `mc7` normal frames: 2026-10-22 (phase 0). MODCOD 4 normal frames
  across 2000 to 2450 MHz: 2026-10-20. Neither set is in the repository; `tests/data/rx_short7.sc8`
  is a 36 KB excerpt of short7-gain32.
- Host: `host/rx_demod.py run` builds `demod.c` with `cc -O2 -DRX_DEMOD_NORMAL=1` and pushes
  12,288-word units, as the firmware does. A frame counts as exact when its 16,200 or 64,800
  hard bits equal the model codeword (`host/dvbs2.py` on the `host/rx.py` payload).
- Board: receiver S3, firmware `firmware/rx`, radio off, `host/rx.py --replay` with the first
  800,000 samples of short7-gain32 (6 frames), 131 pushes of 6,144 words. The cycle counts are
  the firmware's own per-push log. The "after" build also carried temporary per-stage counters.
- MER: "pilot" is the demodulator's own estimate from the PLHEADER and pilots
  (`rx_frame_t.mer_cdb`). "Data" compares every data-symbol LLR (scale 32) with the codeword;
  its 1/32 LLR step limits it to about 41 dB. LeanDVB: the per-frame PLS MER lines in each
  capture's log (stock receiver, normal frames only).

## Commands

```sh
python3 host/rx_demod.py run CAPTURE.sc8 --modcod 7                         # short frames
python3 host/rx_demod.py run CAPTURE.sc8 --modcod 4 --normal --carrier-khz 2245000
python3 host/rx_twin.py --frames 3584 --check --reference 40                # 30 s twin
python3 tests/test_rx_demod.py
python3 host/rx.py 7 --replay CAPTURE.sc8 --port PORT --callsign KJ5SCH     # board, radio off
python3 host/rx.py 7 --replay CAPTURE.sc8 --skip 3 --port PORT --callsign KJ5SCH   # drop units
python3 host/rx_twin.py --frames 10 --gap 0 0.1 --lead-symbols 20000 --out tone.sc8
cd tests/rx_demod_bench && idf.py build flash monitor                       # kernel bench
```

## Real captures

"Clock" is the demodulator's TX symbol clock against the receiver's sample clock, in ppm; in
brackets, the carrier offset divided by the carrier frequency. The demodulator seeds its clock
from that ratio, so this column only shows the loop holding it; the float fits below test the
relation independently.

| Capture | Frames, MODCOD | MHz | RX gain | Exact | Pilot MER dB | Data MER dB | LeanDVB MER dB | CFO Hz | Clock ppm [CFO/fc] |
|---|---|---:|---:|---:|---|---|---|---:|---|
| short7-gain32 | short, 7 | 2420 | 32 | 22/22 | 29.6 to 30.7 | 28.4 to 28.9 |  | -3,270 | -1.363 [-1.351] |
| short10-gain32 | short, 10 | 2420 | 32 | 21/21 | 26.6 to 30.9 | 27.8 to 29.1 |  | -10,626 | -4.332 [-4.391] |
| mc7-a/gain16 | normal, 7 | 2420 | 16 | 5/5 | 26.9 to 27.9 | 26.2 to 26.9 |  | -2,728 | -1.132 [-1.127] |
| mc7-a/gain24 | normal, 7 | 2420 | 24 | 5/5 | 29.6 to 30.2 | 28.4 to 28.7 |  | -6,575 | -2.726 [-2.717] |
| mc7-a/gain32 | normal, 7 | 2420 | 32 | 5/5 | 26.8 to 30.5 | 28.0 to 28.9 |  | -9,464 | -3.889 [-3.911] |
| mc7-a/gain40 | normal, 7 | 2420 | 40 | 5/5 | 21.9 to 22.4 | 21.1 to 21.2 |  | -11,296 | -4.705 [-4.668] |
| mc7-a/gain48 | normal, 7 | 2420 | 48 | 5/5 | 22.5 to 30.2 | 27.6 to 28.7 |  | -13,073 | -5.260 [-5.402] |
| mc7-b/gain24 | normal, 7 | 2420 | 24 | 5/5 | 28.0 to 30.0 | 27.9 to 28.4 |  | -19,775 | -8.139 [-8.171] |
| mc7-b/gain56 | normal, 7 | 2420 | 56 | 0/5 | 10.9 to 11.3 | 10.5 to 10.6 |  | -16,834 | -6.962 [-6.956] |
| mc7-b/gain64 | normal, 7 | 2420 | 64 | 0/5 | 10.4 to 10.5 | 9.8 to 9.9 |  | -17,901 | -7.413 [-7.397] |
| mc7-b/gain72 | normal, 7 | 2420 | 72 | 0/5 | 10.3 to 10.7 | 9.7 to 9.8 |  | -18,694 | -7.726 [-7.725] |
| mc7-b/gain80 | normal, 7 | 2420 | 80 | 0/5 | 10.0 to 10.4 | 9.6 to 9.7 |  | -19,299 | -7.994 [-7.975] |
| 01-2420-gain12 | normal, 4 | 2420 | 12 | 5/5 | 27.1 to 27.6 | 26.3 to 26.7 | 8.0 to 14.0 | -4,263 | -1.765 [-1.762] |
| 02-2025-gain48 | normal, 4 | 2025 | 48 | 4/5 (6 bits) | 29.8 to 30.2 | 27.7 to 28.7 | 7.4 to 11.4 | -9,326 | -4.645 [-4.605] |
| 03-2067-gain48 | normal, 4 | 2067 | 48 | 5/5 | 29.8 to 30.5 | 28.6 to 28.9 | 9.7 to 13.0 | -12,642 | -6.079 [-6.116] |
| 04-2110-gain48 | normal, 4 | 2110 | 48 | 5/5 | 28.5 to 30.6 | 28.6 to 28.9 | 12.2 to 14.9 | -15,037 | -7.182 [-7.127] |
| 05-2200-gain24 | normal, 4 | 2200 | 24 | 5/5 | 21.3 to 23.2 | 18.1 to 19.8 | 7.8 to 9.3 | -16,930 | -7.729 [-7.695] |
| 06-2245-gain24 | normal, 4 | 2245 | 24 | 5/5 | 27.4 to 29.2 | 27.2 to 28.0 | 11.8 to 12.5 | -18,526 | -8.318 [-8.252] |
| 07-2290-gain24 | normal, 4 | 2290 | 24 | 5/5 | 28.8 to 29.6 | 27.6 to 28.3 | 13.1 to 14.1 | -19,825 | -8.653 [-8.657] |
| 08-2420-gain12 | normal, 4 | 2420 | 12 | 5/5 | 26.0 to 27.1 | 25.6 to 26.1 | 9.1 to 9.7 | -21,742 | -9.003 [-8.984] |
| control-01-2200-gain36 | normal, 4 | 2200 | 36 | 5/5 | 24.0 to 26.4 | 18.7 to 21.1 | 10.8 to 15.3 | -5,721 | -2.570 [-2.600] |
| control-02-2245-gain36 | normal, 4 | 2245 | 36 | 4/5 (7 bits) | 30.0 to 30.5 | 27.6 to 28.9 | 14.2 to 15.8 | -11,775 | -5.226 [-5.245] |
| control-03-2420-gain24 | normal, 4 | 2420 | 24 | 5/5 | 26.6 to 30.1 | 27.6 to 28.6 | 15.3 to 15.9 | -16,619 | -6.806 [-6.867] |
| control-04-2000-gain48 | normal, 4 | 2000 | 48 | 5/5 | 28.9 to 30.0 | 28.1 to 28.5 | 8.3 to 9.4 | -15,370 | -7.670 [-7.685] |
| control-05-2450-gain24 | normal, 4 | 2450 | 24 | 5/5 | 26.6 to 30.0 | 28.0 to 28.4 | 14.7 to 15.7 | -20,294 | -8.203 [-8.283] |
| control-06-2420-gain24 | normal, 4 | 2420 | 24 | 5/5 | 29.1 to 30.0 | 28.2 to 28.5 | 14.1 to 15.2 | -21,272 | -8.821 [-8.790] |

CFO and clock are medians over the capture's frames. Each normal-frame capture holds 5 complete
frames, each short-frame capture 21 or 22.

Gain 40 clips the ADC (305,740 of 3.2 M samples) and still decodes exactly at 21 dB. Gains 56
to 80 saturate: about 10 dB MER and 43 to 366 bit errors per frame, which the LDPC would have
to correct. 05-2200-gain24 is weak (rms 10 LSB in int8): 18 to 20 dB data MER, still 5/5; on
that capture LeanDVB reports 8 to 9 dB and needed 287 LDPC flips.

The two inexact frames are frame 90 in `02-2025-gain48` and `control-02-2245-gain36`. In both
the errors sit in data run 9, symbols 270 to 450, where the float data-aided reference also
falls to 11 to 15 dB MER for about 180 symbols. LeanDVB needed 6 and 12 LDPC flips on the same
captures. Every other data run of those frames measures about 30 dB.

## Board cycles

Demodulator cycles per push on the receiver S3, radio off, from the firmware's push log.
"Before" is the portable C; "after" uses the PIE kernels and defers the lock to the next
PLHEADER, so the search and the timing refine never share a push.

| Measure | Before (5e00e5e) | After (8f4bc1d) |
|---|---:|---:|
| Frames exact | 5/5 | 4/4 |
| All 131 pushes | 11,609,693 | 5,166,604 |
| Worst 12,288-word unit | 418,800 | 127,497 |
| Mean 12,288-word unit | 178,450 | 79,369 |
| Acquisition push, 6,144 words | 192,380 to 300,649 | 55,429 to 60,596 |
| Waiting for the next PLHEADER, per push | | 9,440 to 10,930 |
| Tracking push, mean and max | 81,798 and 164,758 | 44,543 and 81,566 |

The after build decodes one frame fewer because the frame whose header the search finds is
used only to lock. Acquisition costs 9.0 to 9.9 cycles per word; tracking about 116 cycles per
symbol. The after build's per-stage counters, over the 103 tracking pushes (39,552 symbols):

| Stage | Cycles | Per symbol |
|---|---:|---:|
| Unpack (`rx_unpack`), all 131 pushes | 1,225,572 | 24 |
| Search, 7 acquisition pushes | 339,536 | |
| Timing refine, once per lock | 62,952 | |
| Matched filter | 2,469,701 | 62 |
| Pilot and header blocks | 287,739 | 7 |
| Release (derotate, LLRs, hard bits) | 636,863 | about 17 per data symbol |

Kernel bench (`tests/rx_demod_bench.c`, 8f4bc1d): each PIE kernel against its portable C
version on random inputs in internal RAM; every output byte matches.

| Kernel | Items | C cycles | PIE cycles | PIE per item |
|---|---:|---:|---:|---:|
| demod_halves (search candidates) | 64 | 1,815 | 791 | 12.36 |
| demod_mf (symbols) | 768 | 1,156,663 | 46,829 | 60.98 |
| demod_turn (data symbols) | 1,440 | 125,310 | 6,329 | 4.40 |
| demod_llr (data symbols) | 1,440 | 95,968 | 8,488 | 5.89 |
| demod_llr, run of 900 | 900 | 60,250 | 5,336 | 5.93 |

The release costs about 17 cycles per data symbol in the replay against 10.3 for its two
kernels in the bench. The rest is the release scheduling and the LLR and hard-bit stores to the
PSRAM frame buffer; the two were not measured separately.

## Acquisition on noise, a tone and dropped units

Until the receiver locks, every unit runs the header search. It must fit the 184,320 cycles a
unit lasts on any input, or capture falls behind. The first live runs (results/2026-10-22-rx-first)
faulted on noise and on the transmitter's Morse ID: 238k cycles per unit. Two causes, both
measured with temporary per-stage counters (row `stage_counters`):

- The capture task's 4 KB stack had landed in RTC FAST memory: largest_ready was 3,584 B after
  Wi-Fi, so the heap placed the stack there. The search's C sign loop, which loads its arrays
  and spills from the stack, ran at 11.5 cycles per word live against 3.35 in replay. PIE code
  that only stores to the stack was unaffected (1.67 against 1.62). Capture now uses static
  stacks in internal RAM (exp/rx cef72a3).
- The search itself: a 64-candidate block took 61 cycles per candidate, 51 of them in the C
  loop that forms and matches the sign registers. `demod_signs_pie` now forms the 64 signs in
  408 cycles (the C reference, 2,215), the signs are matched a 32-bit word at a time, and the
  scratch lives in the demodulator, shared with the tracking blocks.

A failed deferred lock also left the search position one frame back; the next push searched
134k samples of overwritten ring in one call. In a live run that started on noise one push took
2,672,327 cycles (exp/rx df902d3, not recorded here). The search now restarts at the
newest samples. A jump in the dump index already restarted it, in constant time.

| Run | Firmware | Max cycles per 12,288-word unit | Cycles per word |
|---|---|---:|---:|
| Live, transmitter off, stack in RTC FAST (rx-first) | 312a7cc | about 238,000 | 20 |
| Live, transmitter off, static stack | 0402449 | 120,653 | 9.49 |
| Live, transmitter off, 3 s, two runs | 0f8bea3 | 74,772 and 74,131 | 5.81 and 5.67 |
| Replay, TX-off capture (gain 40) | 8724392 | 128,681 | 10.27 |
| Replay, TX-off capture | 0f8bea3 | 72,990 | 5.74 |
| Replay, TX-off capture, every 3rd push dropped | 0f8bea3 | 68,945 | |
| Replay, twin: 0.1 s keyed +1 MHz tone, then 10 frames | 0f8bea3 | 69,063 during the tone | |

The tone-then-frames replay decodes 9 of 9 frames exact (the first one is used to lock). The
short7 replay at 0f8bea3 still decodes 4 of 4. Its worst unit is the lock, 127,807 cycles: the
refine (63k: timing tries 25k, the header's matched filter 5.5k, carrier fit 10k, header block
and checks 22k) plus tracking. With
every 4th push dropped no frame decodes: the lock waits one frame for the next header, and a
drop resets it. Live runs drop units only when a unit costs more than it lasts.

## Digital twin

`host/rx_twin.py` generates dump words from `host/dvbs2.py` frames and the firmware pulse,
with the measured impairments (header of the file). The reference receiver knows the truth:
full 10-bit samples, true DC, a 10-symbol float RRC at the true symbol times, the true carrier
offset, and amplitude and leak fitted on known symbols. Counts start at the first decoded frame;
the frame before it is used to lock.

| Twin run (QPSK 3/4 short) | Frames exact | Bit errors | Data MER dB | Loss dB |
|---|---:|---:|---|---:|
| 30 s, 18 dB, CFO -16.6 to -24.4 kHz, clock -6.8 to -10.1 ppm | 3,583/3,583 | 0 | 17.3 to 17.5 | 0.22 |
| 40 frames, 10 dB | 0/39 (all locked) | 596 | 9.8 to 9.9 | 0.14 |
| 40 frames, 6 dB | 0/39 (all locked) | 16,801 | 5.7 to 5.8 | 0.14 |
| 40 frames, CFO +24 kHz | 39/39 | 0 | 17.3 to 17.5 | 0.26 |
| 40 frames, carrier unknown, clock 9 ppm | 39/39 | 0 | 17.2 to 17.5 | 0.35 |
| 40 frames, 20 ms keyed tone after 20 frames | 38/38 (1 relock) | 0 | 17.4 to 17.5 | 0.24 |

At 10 and 6 dB the raw bit error rates (9.4e-4 and 2.7e-2) are what QPSK gives at the measured
data MER (1.0e-3 and 2.7e-2); every frame stays locked and reaches the FEC. After the keyed
tone the demodulator finds the next header and locks on the one after it, so the first frame
after the tone is the only one missing.

## Clock against carrier offset

Both radios derive their LO and sample clock from 40 MHz crystals, so the TX symbol clock
against the RX sample clock should equal CFO / f_c. A float analysis (not in the repository)
fitted symbol time t0 + k T and carrier over each frame's 33,282 known symbols with a 10-symbol
RRC at full rate; clock = -(T / 16 - 1). Rows `clock_fit` in `runs.jsonl`.

| Capture | MHz | Frames | Clock ppm (fit) | CFO/fc ppm | Max difference ppm | Fit MER dB |
|---|---:|---:|---:|---:|---:|---|
| 01-2420-gain12 | 2420 | 5 | -1.759 | -1.761 | 0.006 | 17.6 to 17.9 |
| 02-2025-gain48 | 2025 | 5 | -4.606 | -4.606 | 0.005 | 17.2 to 17.6 |
| 03-2067-gain48 | 2067 | 5 | -6.119 | -6.117 | 0.007 | 17.2 to 17.5 |
| 04-2110-gain48 | 2110 | 5 | -7.129 | -7.129 | 0.006 | 16.6 to 16.8 |
| 05-2200-gain24 | 2200 | 5 | -7.688 | -7.697 | 0.040 | 15.4 to 15.8 |
| 06-2245-gain24 | 2245 | 5 | -8.252 | -8.254 | 0.004 | 16.5 to 16.8 |
| 07-2290-gain24 | 2290 | 5 | -8.660 | -8.660 | 0.005 | 15.9 to 16.2 |
| 08-2420-gain12 | 2420 | 5 | -8.987 | -8.986 | 0.004 | 15.7 to 16.0 |
| control-01-2200-gain36 | 2200 | 5 | -2.607 | -2.600 | 0.027 | 17.5 to 18.0 |
| control-02-2245-gain36 | 2245 | 5 | -5.250 | -5.246 | 0.005 | 17.5 to 17.8 |
| control-03-2420-gain24 | 2420 | 5 | -6.870 | -6.869 | 0.009 | 16.7 to 16.8 |
| control-04-2000-gain48 | 2000 | 5 | -7.685 | -7.686 | 0.004 | 16.4 to 16.6 |
| control-05-2450-gain24 | 2450 | 5 | -8.286 | -8.285 | 0.005 | 16.3 to 16.4 |
| control-06-2420-gain24 | 2420 | 5 | -8.791 | -8.793 | 0.006 | 15.9 to 16.2 |
| mc7-a/gain16 | 2420 | 5 | -1.128 | -1.127 | 0.006 | 16.8 to 17.1 |

The fit and CFO / f_c agree within 0.01 ppm on 13 of 15 captures and within 0.04 ppm on the
two noisiest. The fit MER here has no leak term, so it sits near LeanDVB's figure rather than
the demodulator's. With `carrier_khz` set, the demodulator starts its timing loop at this
clock, which lets the loop run with a narrow bandwidth from the first frame.
