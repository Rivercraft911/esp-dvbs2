# Transmitter Wi-Fi causes the rare bad frames

With the transmitter in PHY-solo mode (Wi-Fi deinitialized while keyed), the S3 receiver
decoded 103,453 consecutive short QPSK 3/4 frames at 1 Msym/s with none failed or missing,
over 20 runs of 43.3 s. In ten otherwise identical runs with the transmitter's Wi-Fi left
up, 2 of 51,791 frames failed FEC.

| Transmitter | Runs | Frames | Failed | Missing |
|---|---:|---:|---:|---:|
| Wi-Fi up (default) | 10 | 51,791 | 2 | 0 |
| `--phy-solo` | 20 | 103,453 | 0 | 0 |

Earlier runs with the transmitter's Wi-Fi up (results/2026-10-22-rx-continuous and later
stability runs) gave about 6 failed frames in 141k, 4.3 per 100k. At that rate 0 in 103,453
would occur with probability e^-4.4, about 1%.

Both failed frames came from the same model frame, 3,226, in two different runs: 14 and 15
wrong bits in four clusters about 45 symbols apart, symbols 7,499 to 7,637 of the frame. That
is about 140 us around 27 s after the data started. Their LLRs were moderately confident
(|LLR| 12 to 14 against 32 for correct bits); the frames on either side were clean at about
30 dB MER. The transmitter's encoder is not at fault: a host build of the same C encoder
reproduces the model codeword for frames 3,220 to 3,231. A short disturbance of the
transmitted signal, for example periodic PHY work under Wi-Fi, fits both observations; the
mechanism inside Wi-Fi is not identified.

Setup: transmitter 73473df (XIAO, `dvb 2420 7 58 0 64 --short`, with `--phy-solo` in the
second set), coax and 30 dB attenuation, receiver started 2 s before keying with
`rx 2420 32 50 7`. The receiver ran without Wi-Fi: built from dae5e3c plus the changes later
committed as 82cf1ad, so it reports `dae5e3c-dirty`. `runs.jsonl` holds each receiver
summary, the two failed frames' soft bits and the transmitter records.
