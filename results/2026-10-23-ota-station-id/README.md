# Station ID decoded from the air

**Result.** All ten Morse IDs of the five keyed SPI-input runs decoded from the B200 captures
as KJ5SCH: the opening and the closing ID of each run, 23 marks each. Until now the records
checked the ID only from radio-off dumps and measured its tone level on a B200; none decoded it
from RF.

The carrier moved while keyed. Against the B200's own reference, the ID tone (nominally 1 MHz
above the carrier) sat 22.1 to 25.8 kHz low at the start of the opening ID and 30.8 to
31.9 kHz low in the closing ID, 40 to 65 s later. That is -9.1 to -13.2 ppm at 2420 MHz. It
drifted fastest in the first seconds: -92 to -156 Hz/s during the opening ID, -10 to -47 Hz/s
during the closing one. Both oscillators contribute, and the B200's error is unknown.

## Method

- Captures: the kept sc16 captures of the runs in
  [ota-spi-input](../2026-10-23-ota-spi-input/README.md) (a, b, c, d, e): 10 MS/s at 5 Msym/s,
  20 MS/s at 10 Msym/s, B200 TX/RX port, gain 30 dB, 40 dB of marked attenuation.
- Windows: 0 to 6.5 s for the opening ID, and 6.5 s ending 0 to 1 s after the run's end for
  the closing ID.
- The tone drifts by more than the 150 Hz filter of `host/morse.py` allows, so the frequency
  was tracked first: the strongest bin within 150 kHz of +1 MHz in each 6.5 ms segment
  (65,536-point Hann FFT, parabolic peak), a straight-line fit through the key-down segments,
  the capture mixed down along that line, decimated to 10 kS/s, then `morse.decode` at tone 0.
  The fit residual was 3.9 to 13.5 Hz RMS.
- Image and carrier levels are relative to the tone, from the spectrum summed over the window
  (key-up time included), so they read the radio's image and leak at the ID's own settings.

## IDs

| Run | ID | Text | Tone offset, start | Tone offset, end | Drift | Image | Carrier leak |
|---|---|---|---:|---:|---:|---:|---:|
| a | opening | KJ5SCH | -22.10 kHz | -22.75 kHz | -156 Hz/s | -38.7 dBc | -29.8 dBc |
| a | closing | KJ5SCH | -31.01 kHz | -31.21 kHz | -47 Hz/s | -42.1 dBc | -27.6 dBc |
| b | opening | KJ5SCH | -24.43 kHz | -24.94 kHz | -123 Hz/s | -37.8 dBc | -28.9 dBc |
| b | closing | KJ5SCH | -31.56 kHz | -31.66 kHz | -25 Hz/s | -33.4 dBc | -28.0 dBc |
| c | opening | KJ5SCH | -25.27 kHz | -25.75 kHz | -116 Hz/s | -38.3 dBc | -28.9 dBc |
| c | closing | KJ5SCH | -31.82 kHz | -31.91 kHz | -22 Hz/s | -35.8 dBc | -26.0 dBc |
| d | opening | KJ5SCH | -25.75 kHz | -26.19 kHz | -105 Hz/s | -39.0 dBc | -28.3 dBc |
| d | closing | KJ5SCH | -30.79 kHz | -30.98 kHz | -47 Hz/s | -42.0 dBc | -26.6 dBc |
| e | opening | KJ5SCH | -25.64 kHz | -26.02 kHz | -92 Hz/s | -39.1 dBc | -28.2 dBc |
| e | closing | KJ5SCH | -31.00 kHz | -31.05 kHz | -10 Hz/s | -42.7 dBc | -25.4 dBc |

Runs a, b and c keyed for 70 s, d for 45 s and e for 50 s, back to back with a few minutes
between them. The opening offsets grew from run a to run e, which fits a board that had not
cooled down between runs; that is a guess, not a measurement.

## Records

`runs.jsonl`: one record per ID, with the fitted track, levels and the decode.
