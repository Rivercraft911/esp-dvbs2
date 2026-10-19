# Measurements

The chip's dumped samples and the recovered [over-the-air payloads](#over-the-air) decode
byte-exact against the host model in LeanDVB. Keyed runs in a closed lab Faraday box met
the bank deadlines. Capability-table bandwidths are nominal shaped bandwidths;
the measured transmit-filter passband is about 20 MHz.

All board records below used an ESP32-S3 at 240 MHz. The copied JSON lines are unchanged
subsets of the development archive or of later board runs, including their original firmware
versions and command names. [sources.json](measurements/sources.json) records their origins
and SHA-256 hashes.
Historical development evidence remains in
[archive/dev-2026-10-17](https://github.com/Rivercraft911/esp-dvbs2/tree/archive/dev-2026-10-17).

## Over the air

The S3 transmitted at **2420 MHz** to a receive-only ANTSDR E200 running Pluto firmware,
both inside a closed Faraday box. LeanDVB recovered the synthetic payloads byte-exact
against the host model. Frames below count recovered payload frames; MER is the reported
median. Roll-off is 0.35. The DAC ran at 40 MS/s for 1–10 Msym/s and 80 MS/s for 20 Msym/s.
The [public example](../examples/over-the-air/README.md) supplied the libiio run.

| Mode | Symbol rate | Capture / drive | Frames | MER, dB |
|---|---:|---|---:|---:|
| [QPSK 1/2](measurements/ota/ota1-qpsk12-1m/report.json) | 1 Msym/s | peak 128, gain 0 | 587 | 16.7 |
| [QPSK 3/4](measurements/ota/ota2-qpsk34-5m/report.json) | 5 Msym/s | peak 128, gain 4 | 1,436 | 20.5 |
| [QPSK 3/4, public example](measurements/ota/pluto-backend-qpsk34-5m/report.json) | 5 Msym/s | `--backend libiio`; peak 128, gain 0 | 2,937 | 20.9 |
| [QPSK 3/4](measurements/ota/ota4-qpsk34-10m-burst/report.json) | 10 Msym/s | burst; peak 128, gain 4; zero LDPC flips | 124 | 21.6 |
| QPSK 9/10† | 10 Msym/s | — | 124 | 22.4 |
| QPSK 9/10† | 20 Msym/s | 27 MHz nominal; peak 48, decodes at 32–64 | not supplied | 14.2 |
| [QPSK 9/10, overdriven](measurements/ota/ota5-qpsk910-20m/report.json) | 20 Msym/s | peak 128, gain 4; no payload decoded | 0 | — |
| QPSK VCM, 1/2 → 3/4 → 9/10† | 5 Msym/s | rate switching | 2,936 | — |
| Mixed QPSK/8PSK VCM† | 5 Msym/s | modulation switching every frame; 21 MB recovered | 3,517 | — |

† [Operator's run summary](measurements/ota/operator-summary.json); corresponding raw reports
were not supplied in this snapshot. The copied [QPSK VCM report](measurements/ota/vcm-qpsk-5m/report.json)
records 2,938 frames and 20.5 dB MER, differing from the summary's 2,936-frame count.
Full-capture FEC totals can include failed acquisition, Morse-ID and silence hypotheses;
byte-exact recovered payloads do not establish lossless RF capture.

At 80 MS/s, peak 128 overdrives the radio: the 20 Msym/s signal is non-linear and
undecodable. Reducing peak to 32–64 restores decoding; **48 was best**, at 14.2 dB MER.

A ±30 MHz chirp at 80 MS/s, received with the E200 tuned to **2405, 2420 and 2435 MHz**,
stitched together only the central ±9 MHz of each capture. The S3's transmit filter
is about **−3 dB at ±10 MHz and −20 dB by ±14 MHz** from center.

![S3 transmit-filter response stitched from three E200 receiver tunings](measurements/stitch.png)

The 26.67 Msym/s (36 MHz) and 33.33 Msym/s (40 MHz) modes generate correct on-chip
samples and meet their deadlines keyed, but radiate through this ~20 MHz filter and
do not decode over the air yet. A wider HT40 setting is being investigated;
the [current comparison](measurements/ht40-compare.png) does not show a wider passband.
The README's [5 Msym/s spectrum](measurements/qpsk34-5m-spectrum.png) is a real RF capture;
it includes station ID and silence and is not a calibrated power or emission-mask measurement.

## What it can do

These are the README's capability modes and timing records. Values are rounded;
exact selected runs follow.
Keyed means 30 s including station ID in the shielded box. Radio off means the encoder
and bank schedule ran in real time without DAC playback. Different rows can refer to
different firmware versions. The video-example timing row is a host-model TS check;
the RF burst above used synthetic payloads.

| Mode | Symbol rate | Bandwidth | Data rate | Tested | Fill headroom |
|---|---:|---:|---:|---|---|
| 8PSK 9/10 | 26.67 Msym/s | 36 MHz | 69.8 Mb/s | keyed | 17.5%; encoder 20.5% ([records](#8psk-at-36-mhz)) |
| 8PSK 5/6 | 26.67 Msym/s | 36 MHz | 64.6 Mb/s | keyed | 17.8%; encoder 11.6% ([records](#8psk-at-36-mhz)) |
| QPSK 9/10, roll-off 0.20 | 33.33 Msym/s | 40 MHz | 58.2 Mb/s | keyed | 16%; encoder 37%, prepared payload ([records](#40-mhz)) |
| 8PSK 2/3 | 26.67 Msym/s | 36 MHz | 51.6 Mb/s | keyed | 16.5%; encoder 13.9% ([records](#8psk-at-36-mhz)) |
| 8PSK 5/6 | 20 Msym/s | 27 MHz | 48.4 Mb/s | keyed | 14% [radio off, current pulse](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-17-next4/README.md); 5% keyed, [earlier pulse](#8psk-at-20-msyms) |
| QPSK 9/10 | 26.67 Msym/s | 36 MHz | 46.6 Mb/s | keyed | 18% ([record](#qpsk-at-2667-msyms)) |
| 8PSK 2/3 | 20 Msym/s | 27 MHz | 38.7 Mb/s | keyed | 14% radio off, current pulse |
| QPSK 9/10 | 20 Msym/s | 27 MHz | 34.9 Mb/s | keyed | 22% ([record](#qpsk-at-2667-msyms)) |
| 8PSK 5/6 | 10 Msym/s | 13.5 MHz | 24.2 Mb/s | keyed | 49% ([archive](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-16-keyed-next2/README.md)) |
| QPSK 9/10 | 10 Msym/s | 13.5 MHz | 17.5 Mb/s | radio off | 48% ([archive](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-17-cleanup/README.md)) |
| QPSK 3/4, current video example | 10 Msym/s | 13.5 MHz | 14.5 Mb/s | host-model TS dry run | — ([example](../examples/live-video/README.md#rf-free-dry-run)) |
| 8PSK 9/10 | 5 Msym/s | 6.75 MHz | 13.1 Mb/s | radio off | 55% ([archive](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-17-qpsk-goal/README.md)) |
| 8PSK 2/3 | 5 Msym/s | 6.75 MHz | 9.68 Mb/s | keyed | 57% ([archive](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-16-keyed-next2/README.md)) |
| QPSK 1/2 | 1 Msym/s | 1.35 MHz | 0.97 Mb/s | keyed | 52% ([record](measurements/2026-10-16-keyed-qpsk-1m.jsonl), [report](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-16-keyed-next2/README.md)) |

Fill headroom is `100 * (1 - max_fill_cycles / bank_deadline_cycles)`, rounded to the
nearest percent. Encoder headroom is `100 * (1 - mean_encoder_cycles / frame_duration_cycles)`.
Recorded minimum slack also includes scheduling overhead and is reported separately below.

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

## 8PSK at 36 MHz

On 2026-10-18, firmware **eee5153** keyed 8PSK 2/3, 5/6 and 9/10 at
80/3 Msym/s, roll-off 0.35, for 30 s each in the closed Faraday box. The commands
were `dvb 2412 MC 26m667 30 0 128 --fixture`, with MC 13, 15 and 17. Wi-Fi
remained up. Each run reused one prepared payload and encoded every frame
through the normal baseband, FEC and pilot stages. This measures the complete
transmitter's timing with prepared input, not external input throughput or RF decoding.

| Quantity | [2/3, MODCOD 13](measurements/2026-10-18-8psk-26m667-keyed.jsonl) | [5/6, MODCOD 15](measurements/2026-10-18-8psk-26m667-keyed.jsonl) | [9/10, MODCOD 17](measurements/2026-10-18-8psk-26m667-keyed.jsonl) |
|---|---:|---:|---:|
| Status | ESP_OK | ESP_OK | ESP_OK |
| Data frames played | 23,501 | 23,501 | 23,501 |
| Startup / steady-state dummy frames | 2 / 0 | 2 / 0 | 2 / 0 |
| Late fills / switches | 0 / 0 | 0 / 0 | 0 / 0 |
| Encoder mean / live maximum, cycles/frame | 172,076 / 239,463 | 176,477 / 251,019 | 158,719 / 213,647 |
| Encoder spare | 13.9% | 11.6% | 20.5% |
| Longest bank fill, cycles | 39,419 | 38,803 | 38,963 |
| Fill spare | 16.5% | 17.8% | 17.5% |
| Minimum fill slack, cycles | 7,499 | 7,779 | 7,950 |
| Minimum queued lead, symbols | 692 | 692 | 3,252 |
| Queue pages | 54 | 54 | 54 |
| Minimum free internal RAM during command, B | 32,087 | 29,427 | 27,731 |

The frame duration is `22194 * 240e6 / (80e6/3) = 199746 cycles` and the
bank deadline is 47,220 cycles. **9/10 meets the 15% reserve on both cores.**
2/3 and 5/6 run at full rate with **14% and 12% mean encoder spare**; their
fills retain more than 15%. All three had zero steady dummies, empty queues or
underruns. The same image passed the 23/23 self-check and the SPI and USB checks.

The instrumented radio-off pace adds profiling. Over 20 s per mode it recorded
3,472 / 4,262 / 100 steady dummies, encoder means of 179,099 / 183,223 / 163,883
cycles/frame and longest fills of 42,726 / 42,697 / 42,554 cycles. It failed the
experimental 15% reserve gate; keyed runs had no steady dummies.
[Copied pace records](measurements/2026-10-18-8psk-26m667-pace.jsonl).

The public [self-check](../examples/self-check/README.md) uses the keyed figures
above plus fixed profiling allowances: 4,500 cycles for fills and 10,000 cycles
for encoder mean/live maximum. Its fill ceilings are 43,919 / 43,303 / 43,463,
and mean encoder ceilings 182,076 / 186,477 / 168,719 cycles. These leave less
than 15% reserve and are regression limits, not new board measurements. Steady
dummies are capped at 180 / 220 / 6 per payload second, with at most four startup
dummies and one cold production-bound miss. A matching minimum-data-frame gate
rejects excessive lost airtime even if dummy counters are wrong. Missed bank
deadlines, queue starvation and minimum internal RAM below 20,000 B always fail.

Earlier firmware **87b17a9** averaged 163,086 / 167,875 / 147,941 encoder
cycles/frame, with longest fills of 39,011 / 38,509 / 38,255 cycles and zero
steady dummies ([earlier keyed records](measurements/2026-10-18-8psk-26m667-87b17a9.jsonl)).
Restoring the older modes' RAM and moving the control code into locked instruction
cache cost about 9k encoder cycles/frame; those earlier timings do not describe eee5153.

With B = 5370, 6720 and 7264 bytes and Ns = 22194 symbols, the payload-rate
equation above gives 51.6, 64.6 and 69.8 Mb/s during data playout. Nominal shaped
bandwidth is `1.35 * (80/3) MHz = 36 MHz`. The 30 s command includes both
Morse IDs; these rates exclude their airtime.

Separate 16-frame USB sample dumps at these three rates decoded as contiguous
exact payloads in LeanDVB, with zero LDPC flips, BCH failures, BCH corrections
or payload bit errors. [Copied decode records](measurements/2026-10-18-8psk-26m667-decodes.jsonl).
These were digital dumps, not receiver captures over RF.

The [queue-depth sweep](measurements/2026-10-18-8psk-page-sweep.jsonl), firmware
**aaeb50e**, paced MODCOD 17 for 10 s per count with Wi-Fi and the radio off.
At 46, 50, 54 and 58 pages it played 9,374, 11,238, 12,015 and 12,015 data frames,
with 17,602, 5,180, 0 and 0 steady dummies. This led to the 54-page layout used
by the keyed runs; it does not establish the minimum queue depth under every load.

## 40 MHz

On 2026-10-18, firmware **40fa63d** on `exp/phy-solo` ran QPSK 9/10 at 100/3 Msym/s,
roll-off 0.20, with the same prepared payload in radio-off pacing and two 30 s keyed
runs inside the closed box. The keyed runs used 2412 MHz, gain 0 and peak 128.
All three reported ESP_OK, zero late fills, late switches, fatal lateness, underruns
and empty-queue events.

| Quantity | [Radio off, Wi-Fi running](measurements/2026-10-18-qpsk-fixture-pace.jsonl) | [Keyed, Wi-Fi running](measurements/2026-10-18-qpsk-fixture-keyed-wifi.jsonl) | [Keyed, PHY solo](measurements/2026-10-18-qpsk-fixture-keyed-solo.jsonl) |
|---|---:|---:|---:|
| Encoder mean / live maximum, cycles/frame | 152,754 / 174,627 | 150,667 / 170,229 | 150,669 / 168,743 |
| Encoder kernel mean, cycles/frame | 146,992 | 145,365 | 145,366 |
| Longest bank fill, cycles | 44,649 | 39,448 | 39,421 |
| Minimum fill slack, cycles | 2,276 | 7,368 | 7,395 |
| Minimum queued lead, symbols | 40,320 | 41,600 | 41,600 |
| Minimum free internal RAM during command, B | 29,327 | 31,171 | 31,171 |
| Free internal RAM during playback, B | 31,179 | 31,171 | 46,475 |

The commands were `dvbpace 11 33m333 30 128 0`,
`dvb 2412 11 33m333 30 0 128 --fixture`, and the same keyed command with `--phy-solo`.
The fixture copies one prepared payload in 1316-byte chunks and encodes every frame;
external input transport is outside this test. Pacing enables extra profiling and runs
30 s of payload plus both IDs; keyed duration includes the IDs. Means compare per-frame
encoder work, excluding waits for queue space.

Keying added no measurable encoder cost here. Removing Wi-Fi changed the keyed mean
by two cycles and reclaimed 15,304 internal bytes during playback. With Wi-Fi running,
the encoder used 150,667 of 239,630.4 cycles/frame (**37% mean headroom**); the longest
fill used 39,448 of 47,220 cycles/bank (**16% headroom**).

The earlier **e96795b** run generated fresh xorshift payload bytes for every frame.
It keyed QPSK 9/10 at 100/3 Msym/s, roll-off 0.20, for 30 s inside the
shielded box with `dvb 2420 11 33m333 30 4 128` (firmware revision read with `info` just
before the run). Status was ESP_OK, with zero late fills, late switches, fatal lateness,
underruns or empty-queue events.
[Copied keyed record](measurements/2026-10-17-qpsk-reserve-keyed.jsonl).

| Quantity | e96795b, keyed | 15% reserve line |
|---|---:|---:|
| Bank deadline | 47,220 cycles | |
| Longest bank fill | 39,548 cycles | 40,137 |
| Minimum fill slack | 7,394 cycles | |
| Encoder mean / live maximum, cycles/frame | 198,879 / 222,440 | 203,686 mean |
| Frame duration, derived | 239,630.4 cycles | |
| Minimum queued lead | 34,901 symbols | |
| Minimum free internal RAM during command | 30,395 B | 20,000 B |

Minimum RAM figures use `wifi_min_free`, the low-water mark since command start,
including Wi-Fi startup. The new records also sample free RAM during playback.

Its radio-off mean of 152,128 cycles/frame used a prepared payload. Comparing that with
the keyed mean of 198,879 mixed payload sources: the roughly 46.8k-cycle gap came from
synthetic payload generation, not keying. The historical run still met the 15% reserve
on both cores with that work included. Spectrum, filter flatness near ±20 MHz, RF decoding
and live input at this rate remain unmeasured.
[Sign-window design](how-it-works.md#sign-windows-and-40-mhz).

## QPSK at 26.67 Msym/s

Firmware **3865c75** keyed QPSK 9/10 for 30 s at both 80/3 and 20 Msym/s in
the closed Faraday box. Both runs had 152,478 bank fills, zero late fills, late
switches, fatal lateness or underruns. The duration includes both IDs.
[Copied records](measurements/2026-10-17-keyed-qpsk-runs.jsonl),
[original report](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-17-keyed-qpsk/README.md).

| Symbol rate | Longest fill | Minimum slack | Encoder mean / maximum | Minimum free internal RAM during command |
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

| Code rate | Longest fill | Minimum slack | Encoder mean / maximum | Minimum free internal RAM during command |
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
git fetch origin tag archive/dev-2026-10-17  # if the archive tag is absent
python3 host/plots.py
python3 host/plots.py --check               # verify sources without rendering
```

All five figures use the point table, checked against exact JSONL lines and file hashes
before drawing. Archived sources are read with `git show`; copied records retain their
[original provenance](measurements/sources.json). Cycle time uses 240 MHz; bank periods
are 85,608 cycles (356.7 µs) early and 47,220 (196.75 µs) later, from the archived reports
quoted in the table. Payload rates follow the formula above and exact sample ratios.
Historical records are never relabeled as new runs. No board connection is needed.

Findings and licences from the projects this work builds on are credited in
[prior-art.md](prior-art.md).
