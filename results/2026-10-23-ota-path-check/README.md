# Path check before resuming: no signal at the B200

**Result.** The first keyed run after the bench was rebuilt did not reach the B200 through the
coax. I stopped keying after this one run. The Morse ID tone arrived 75.2 dB weaker than in the
earlier runs, where it should have been about as strong, and the 8PSK data was not visible
above the noise. LeanDVB decoded nothing. The signal reached the B200 only by leakage, so the
transmitter's coax was not on the B200's TX/RX port at the time. Where it went was not checked.

## The run

- Transmitter: XIAO ESP32-S3 44:B1:76:B0:E5:8C, firmware 517c641, callsign KJ5SCH.
  `dvb 2420 13 5m 15 0 128`: 8PSK 2/3, 5 Msym/s, 15 s with both IDs, gain 0, peak 128. The
  board reported `ESP_OK`, 1,136 frames played and no late fill.
- Path as described to me: coax with 30 dB of marked attenuation to the B200 (34A6B8E), TX/RX
  port, receive only. B200 gain 20 dB, 10 MS/s, continuous capture under
  `caffeinate -i -s -d`, no overflow.
- Expected level: the earlier path had 40 dB at B200 gain 30. 10 dB less attenuation and
  10 dB less gain should give the same level in dBFS.

## Measured

| | Earlier (ota-spi-input run a) | This run |
|---|---:|---:|
| ID tone peak | -7.4 dBFS | -82.6 dBFS |
| In-band PSD during data | -82.5 dBFS/Hz | -133.5 dBFS/Hz |
| In-band PSD before keying | about -131 dBFS/Hz | -133.9 dBFS/Hz |

The tone peak is the strongest bin within 150 kHz of +1 MHz in 65,536-point Hann FFTs, the
median of the 20 strongest segments between 0.5 and 5.5 s. In this run the tone was still
found at +977.4 kHz, about 26 dB above the median bin, the same frequency as the earlier runs'
ID; the data stayed at the noise floor (-133.5 against -133.9 dBFS/Hz before keying).

## Records

`runs.jsonl`: the run record (board completion record, B200 settings and capture status,
LeanDVB report) with the tone comparison.
