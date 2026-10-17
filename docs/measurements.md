# Measurements

The chip's dumped samples decode bit-exact in LeanDVB. Keyed runs in a closed lab
Faraday box met the bank deadlines. **No receiver has yet decoded this transmitter over RF.**
Bandwidth below is the nominal shaped bandwidth, not a measured RF passband or emission mask.

All board records below used an ESP32-S3 at 240 MHz. The copied JSON lines are unchanged
subsets of the development archive, including their original firmware versions and command
names. [sources.json](measurements/sources.json) records their origins and SHA-256 hashes.
The complete evidence remains in
[archive/dev-2026-10-17](https://github.com/Rivercraft911/esp-dvbs2/tree/archive/dev-2026-10-17).
No hardware was accessed for the public cleanup.

## What it can do

This is the README's capability table. Values are rounded; exact selected runs follow.
Keyed means 30 s including station ID in the shielded box. Radio off means the encoder
and bank schedule ran in real time without DAC playback. Different rows can refer to
different firmware versions; these are historical capabilities, not a new acceptance run.

| Mode | Symbol rate | Bandwidth | Data rate | Tested | Slack |
|---|---:|---:|---:|---|---|
| QPSK 9/10, roll-off 0.20 | 33.33 Msym/s | 40 MHz | 58.2 Mb/s | keyed | 12% fill, encoder about 1% ([record](#40-mhz)) |
| 8PSK 5/6 | 20 Msym/s | 27 MHz | 48.4 Mb/s | keyed | 14% [radio off, current pulse](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-17-next4/README.md); 4% keyed, [earlier pulse](#8psk-at-20-msyms) |
| QPSK 9/10 | 26.67 Msym/s | 36 MHz | 46.6 Mb/s | keyed | 18% ([record](#qpsk-at-2667-msyms)) |
| 8PSK 2/3 | 20 Msym/s | 27 MHz | 38.7 Mb/s | keyed | 14% radio off, current pulse |
| QPSK 9/10 | 20 Msym/s | 27 MHz | 34.9 Mb/s | keyed | 22% ([record](#qpsk-at-2667-msyms)) |
| 8PSK 5/6 | 10 Msym/s | 13.5 MHz | 24.2 Mb/s | keyed | 49% ([archive](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-16-keyed-next2/README.md)) |
| QPSK 9/10 | 10 Msym/s | 13.5 MHz | 17.5 Mb/s | radio off | 48% ([archive](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-17-cleanup/README.md)) |
| 8PSK 9/10 | 5 Msym/s | 6.75 MHz | 13.1 Mb/s | radio off | 55% ([archive](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-17-qpsk-goal/README.md)) |
| 8PSK 2/3, the video mode | 5 Msym/s | 6.75 MHz | 9.68 Mb/s | keyed | 57% ([archive](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-16-keyed-next2/README.md)) |
| QPSK 1/2 | 1 Msym/s | 1.35 MHz | 0.97 Mb/s | keyed | 52% ([archive](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-16-keyed-box/README.md)) |

Roll-off α is 0.35 unless noted. For symbol rate Rs in symbols/s, payload bytes B
and Ns symbols per normal frame with pilots:

```
nominal bandwidth = (1+α) * Rs                       Hz
payload rate      = 8*B * Rs / Ns                   bit/s
```

`B = (Kbch-80)/8` comes from the baseband header length and coding parameters in
[EN 302 307-1 V1.4.1, §§5.1, 5.3, 5.5, 5.6 and Annex A](https://www.etsi.org/deliver/etsi_en/302300_302399/30230701/01.04.01_60/en_30230701v010401p.pdf).
`Ns = 33282` for QPSK or `22194` for 8PSK. The retained records report both B and Ns.
For QPSK 9/10, `8*7264*(100e6/3)/33282 = 58.2016e6 bit/s` and
`1.20*(100/3) MHz = 40 MHz`. As Rs goes to zero, both rates go to zero.
These payload rates exclude Morse ID intervals and include the synthetic callsign header;
they are not measured sustained video-input throughput.

![Payload rates calculated from the five selected keyed records](img/payload.png)

## 40 MHz

Firmware **51764f0** keyed QPSK 9/10 at 100/3 Msym/s, roll-off 0.20, for 30 s
inside the shielded box. The wide filter setting was read back. Status was ESP_OK,
with zero late fills, late switches, fatal lateness or underruns.
[Copied keyed record](measurements/2026-10-17-moonshot-keyed.jsonl),
[full report and radio-off checks](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-17-moonshot/README.md).

| Quantity | Recorded value |
|---|---:|
| Bank deadline | 47,220 cycles |
| Longest bank fill | 41,130 cycles |
| Minimum fill slack | 5,810 cycles |
| Encoder mean / live maximum | 237,370 / 265,333 cycles/frame |
| Frame duration, derived | 239,630.4 CPU cycles |
| Minimum queued lead | 5,120 symbols |
| Free internal RAM with Wi-Fi up | 28,667 B |

The queue absorbed encoder frames longer than their individual duration. The encoder
averaged about 99% of a frame. This meets the observed schedule, but fails the proposed
15% reserve target. Spectrum, filter flatness near ±20 MHz, RF decoding and live input
at this rate remain unmeasured. [Sign-window design](how-it-works.md#sign-windows-and-40-mhz).

## QPSK at 26.67 Msym/s

Firmware **3865c75** keyed QPSK 9/10 for 30 s at both 80/3 and 20 Msym/s in
the closed Faraday box. Both runs had 152,478 bank fills, zero late fills, late
switches, fatal lateness or underruns. The duration includes both IDs.
[Copied records](measurements/2026-10-17-keyed-qpsk-runs.jsonl),
[original report](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-17-keyed-qpsk/README.md).

| Symbol rate | Longest fill | Minimum slack | Encoder mean / maximum | Free RAM, Wi-Fi up |
|---|---:|---:|---:|---:|
| 26.67 Msym/s | 38,810 cycles | 8,131 cycles | 226,587 / 524,436 cycles | 41,387 B |
| 20 Msym/s | 36,620 cycles | 10,318 cycles | 226,682 / 312,494 cycles | 30,579 B |

These runs predate the wide-filter selection. They establish timing; attenuation
of the band edges was unknown. `20mq4` in the original record is today's QPSK `20m` kernel.

## 8PSK at 20 Msym/s

Firmware **5dbfcea** keyed 8PSK 2/3 and 5/6 for 30 s each at 20 Msym/s in the
closed Faraday box. All late-fill, late-switch, fatal and underrun counters were zero.
[Copied records](measurements/2026-10-16-keyed-20m-runs.jsonl),
[original report](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-16-keyed-20m/README.md).

| Code rate | Longest fill | Minimum slack | Encoder mean / maximum | Free RAM, Wi-Fi up |
|---|---:|---:|---:|---:|
| 2/3 | 45,092 cycles | 1,842 cycles | 166,233 / 488,765 cycles | 21,211 B |
| 5/6 | 45,068 cycles | 1,869 cycles | 184,201 / 429,293 cycles | 21,691 B |

These keyed records used the earlier nine-symbol pulse and predate wide-filter selection.
Today's six-symbol pulse has faster radio-off measurements and exact digital decode,
but has not been qualified by a keyed receiver. The 14% table figure comes from the
[current-pulse radio-off run](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-17-next4/README.md).

![Measured longest bank fill versus the deadline for the selected keyed records](img/fill.png)

## One independent decode

At 26.67 Msym/s, LeanDVB recovered **14 contiguous QPSK 9/10 frames out of a
16-frame board dump**, starting at frame zero: payload match true, zero payload errors,
zero LDPC bit flips and zero BCH failures. Digital modulation error ratio (MER) was
37.98 dB. This is a USB sample decode, before RF.
[Copied receiver summary](measurements/2026-10-17-qpsk-fill-board-decodes.jsonl),
[full report, firmware e50e794 and dump CRC 1271511239](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-17-qpsk-fill-board/README.md).
That early paced run used pre-encoded frames, so its fill timing is not complete-transmitter timing.

The [self-check](../examples/self-check/README.md) reproduces selected dumps with the
current firmware, compares firmware, USB and file CRCs with pinned original waveforms,
and decodes them independently. CI checks all supported modulation/rate combinations
on the host. Neither check substitutes for an RF receiver measurement.

## Recreate the figures

```sh
python3 -m pip install matplotlib
python3 host/plots.py
```

Both figures are generated from the copied keyed records. Cycle time uses the recorded
240 MHz CPU and bank period uses `15740/(80e6 samples/s) = 196.75 µs` from
`radio_s3.h`. The script validates the copied-file hashes before drawing. No chart
uses hand-entered measured values. Historical records are never relabeled as new runs.

The early memory, encoder, signal-quality, station-ID and test-tone measurements in
the [story](story.md) link directly to the archive. Findings and licences from the
projects this work builds on are credited in [prior-art.md](prior-art.md).
