# First RF: DAC bank streaming received on an SDR

The ESP32-S3 streams its own samples out of the Wi-Fi radio at 2420 MHz, and an E200 SDR
receives them. With the transmit chain brought up through the vendor CW tone and the gain then
pinned (`chain cwgain`), a +1 MHz test tone comes out single-sideband: image -35 dBc, carrier
leak about -45 dBc, level repeatable within 0.5 dB over three keyings. The first bring-up method
(`chain forced`) radiated, but only the I branch reached the mixer and the carrier leak was
stronger than the signal.

Bank streaming kept time in every run: 45 keyed runs of 2 to 30 s, 1,070,892 bank switches,
0 late fills and 0 fatal deadline misses. 3353 switches (0.3%) landed after their nominal time,
at most 746 cycles (124 samples) late, inside the 2048-sample overlap where both banks hold the
same samples. In a 0.5 s tone capture (about 1400 switches), the tone's phase folded at the
356.7 µs bank period moves 1.2° peak to peak; one dropped or repeated sample would move it 9°.

## Setup

- Board: ESP32-S3 rev 0.2, 240 MHz. Firmware `eb2788f` for the first run, which stopped before
  keying (`ESP_ERR_INVALID_STATE`: the front end's gain base index was 160, fixed in `b63393f`).
  Then `b63393f`, and from 20:57 UTC `b63393f` plus the working-tree changes committed just before
  these results (diagnostic signals, the `chain` command). The build stamps `b63393f` for both
  because the version string does not mark a dirty tree.
- RF path: board U.FL, pigtail, attenuators, E200 RX1. The attenuation value was set on the bench
  (at least 30 dB per [docs/hardware.md](../../docs/hardware.md)) and is not recorded by the tools.
- Receiver: E200 (AD9364, PlutoSDR firmware v0.34) over Ethernet, libiio 0.25, RX LO 2420 MHz,
  manual gain, E200 transmit LO powered down. Settings were read back before capturing. Samples
  are 12-bit in int16, so amplitudes below are in ADC counts (full scale 2048).
- Board command `tx 2420 <signal> <seconds> <gain>`. DAC 40 MS/s. `tone` is +1 MHz at amplitude
  256 (of 511); the other signals are listed in `firmware/components/radio_s3/radio_signals.c`.
- The last run (`default-tone128`) checks that `cwgain` is the default after a reset.
- `runs.jsonl`: every board record. `captures.jsonl`: one line per capture with SDR settings and
  the analysis from `host/capture.py`. The raw captures (8 MB each) are not in git.

## The forced bring-up passes only the I branch

`chain forced` keys the front end directly: workmode, TX power-up, `force_tx_gain()` into gain
slot 0, `force_txon_mode()` with the slot offset cancelling the base index. Gain 4, RX gain 20 dB,
amplitudes in ADC counts:

| Signal (DAC words) | Carrier | +1 MHz | -1 MHz |
|---|---:|---:|---:|
| zero | 66.1 | 0.02 | 0.02 |
| tone: I = 256 cos, Q = 256 sin | 55.3 | 25.5 | 24.5 |
| ionly: I = 256 cos, Q = 0 | 70.5 | 33.8 | 32.4 |
| qonly: I = 0, Q = 256 sin | 79.1 | 0.02 | 0.02 |
| dci, dcq: constant 200 on I or Q | 67.9, 57.1 | 0.02 | 0.02 |
| low: tone at 64 | 73.1 | 1.06 | 1.06 |

Nothing on Q reaches the output, so the tone becomes a double sideband from I alone, sitting on
a carrier leak larger than the signal. A quarter of the amplitude gives 1/24 of the sideband,
so the I path is not linear either. The carrier also wanders between runs (38 to 81 counts).

## The CW bring-up fixes it

`chain cw` starts the vendor carrier-wave test (`esp_phy_wifi_tx_tone()`, ESP-IDF's cert-test API)
on the nearest Wi-Fi channel, stops its tone with `stop_tx_tone(1)` so the DAC takes the data
path, then moves only the PLL with `phy_set_freq()`. The method is described in esp-sdr-zephyr
(`lib/esp_sdr/src/esp_sdr_paced.c:203-232` at `3214c16`). Gain 4, RX gain 10 dB:

| Signal | Carrier | +1 MHz | -1 MHz |
|---|---:|---:|---:|
| zero | 0.14 (noise) | 0.02 | 0.02 |
| tone | 0.19 | 20.7 | 0.69 |
| neg: -1 MHz | 0.27 | 1.01 | 32.8 |
| ionly | 0.14 | 9.39 | 9.55 |
| qonly | 0.26 | 11.9 | 12.5 |

Both branches work, the tone and its mirror swap places with the sign of Q, and the carrier leak
is near the noise. The plain CW mode leaves the vendor's power control running, though: the same
settings gave tone levels spread over 6.4 dB from run to run, and the loudest runs carried 14%
rms envelope ripple with its strongest line at 610.35 Hz (40 MHz / 65536).

`chain cwgain` then pins the gain as the forced path does (`force_tx_gain()`, slot 0). Three
keyings of the same tone:

| Run | +1 MHz | Image | Carrier leak | Envelope ripple rms | Phase wander rms |
|---|---:|---:|---:|---:|---:|
| 1 | 7.83 | -35.0 dBc | -44.2 dBc | 3.7% | 5.2° |
| 2 | 7.42 | -34.7 dBc | -44.8 dBc | 3.8% | 6.0° |
| 3 | 7.59 | -35.0 dBc | -44.9 dBc | 4.0% | 4.9° |

The ripple numbers here are mostly receiver noise (the tone is 8 counts against about 6 counts
rms of noise across the 4 MHz capture). The 610 Hz line is absent on this tone, but a Q-only
signal under `cwgain` still showed 12.8% at 610 Hz, so some periodic process reacts to envelope
variation. Its effect on a DVB-S2 signal is for the MER measurement to show.

## Drive level

Tone amplitude sweep under `cwgain`, gain 4 (output at +1 MHz, ADC counts):

| DAC amplitude | 16 | 32 | 64 | 128 | 256 | 384 |
|---|---:|---:|---:|---:|---:|---:|
| Output | 1.29 | 2.53 | 4.84, 4.31 | 9.15 | 7.42 | 7.64 |
| vs. linear from 16 | 0 dB | -0.2 dB | -0.6, -1.6 dB | -1.1 dB | -8.9 dB | -12.2 dB |

The chain is within about 1 dB of linear up to amplitude 128, and above that the output falls
instead of saturating. A 20 MS/s capture at 256 shows no -3 MHz product (-56 dBc), so the words
are not clipping per branch; the chain turns its own gain down, and the carrier leak drops 5x at
the same time. The single-tone runs at 256 above were therefore in this reduced state. Signals
should keep DAC peaks near 128 or below and take power from the analog gain. The two-tone run
under `chain cw` (+1 and +3 MHz at 128 each, peak 256) agrees: worst product -23.6 dBc at -1 MHz,
where the +1 MHz image and the 2f1 - f2 product coincide.

## What is not shown yet

- The 40 Mchip/s tag could not be decoded (`captures.jsonl`, "tag timing lost a sample"). The
  transmit filters pass about +-10 MHz in both CW modes (12 to 18 dB down beyond +-12 MHz), and
  the `cw40` variant (`esp_phy_cbw40m_en(true)` first) widens only the upper side. Per-sample
  continuity therefore rests on the tone phase above, not on the tag.
- Lines at +-12.8 to 13.4 kHz from the tone at -32 to -35 dBc, cause unknown, and a line 6 MHz
  above the carrier at about -31 dBc in the 20 MS/s captures, not traced to the board or the SDR.
- The carrier sits 49 to 58 kHz above 2420 MHz (20 to 24 ppm, board and SDR crystals combined).
- No power measurement in dBm; levels are SDR counts behind an unrecorded attenuation.
