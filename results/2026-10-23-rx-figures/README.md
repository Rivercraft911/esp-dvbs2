# What the receiving S3 sees

Two 187.5 ms captures taken by the receiving ESP32-S3 itself, through its own radio, while the
other S3 transmitted into it through coax and 30 dB of attenuation at 2420 MHz. They are the
source of `docs/img/rx-5m.png` and `docs/img/rx-1m.png`.

| Capture | Mode | Frames decoded | Data MER, median | Pilot MER, median | CFO |
|---|---|---:|---:|---:|---:|
| 8psk89-5m | 8PSK 8/9 short, 5 Msym/s, 12.64 Mb/s | 164 of 164 exact | 28.94 dB | 29.80 dB | -2,017 Hz |
| qpsk34-1m | QPSK 3/4 short, 1 Msym/s, 1.39 Mb/s | 21 of 21 exact | 28.78 dB | 30.33 dB | -9,807 Hz |

Transmitter d4957d3: `dvb 2420 16 5m 25 0 128 --short --phy-solo` and
`dvb 2420 7 25 0 64 --short --phy-solo`. Receiver 43ddfe5 (`44:b1:76:b0:da:c8`):
`dump 2420 32 3000000`, started about 8 s after the transmitter, after its Morse ID. Neither dump
dropped a sample (`drops` 0). A dump holds 3,000,000 pairs of int8 I and Q, floor(s10 / 4), which
is exactly what the receiver's `rx_unpack` feeds its demodulator.

The spectrum and spectrogram use nonoverlapping symmetric Hann windows (1,024 samples at
5 Msym/s, 4,096 at 1 Msym/s), power divided by sample rate times window energy, averaged over
pairs of frequency bins and over 8 (5 Msym/s) or 2 (1 Msym/s) windows in time. Levels are dB
relative to int8 full scale, not calibrated RF power. The constellation is the data symbols of
every frame as the receiver's demodulator (`firmware/components/dvbs2rx` at 43ddfe5, built for
the host by `host/rx_demod.py`, the same code the chip runs) leaves them: its soft bits are a
point's coordinates, 32 per nominal axis amplitude (QPSK) or per a sin(pi/8) (8PSK), so the
points are rebuilt exactly from the soft bits, histogrammed in 600 x 600 bins over +-1.3 and
drawn on a log scale. Every frame was compared with the transmitter's model codeword: no bit
differed.

The NPZ files keep these summaries rather than the 6 MB captures; `runs.jsonl` pins the capture
and summary hashes. Regenerate the figures:

```sh
python3 host/plots.py --rf-only
```

With the captures (`<dir>/8psk89-5m/capture.sc8`, `<dir>/qpsk34-1m/capture.sc8`), the summaries
are rebuilt and compared with the committed ones:

```sh
python3 host/plots.py --rf-only --rx-iq-dir <dir>
```
