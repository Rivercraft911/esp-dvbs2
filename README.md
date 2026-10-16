# esp-dvbs2

What started as one hack on the ESP32-C3 has snowballed into a whole family of ESP32 software
defined radios. People have pulled raw I/Q samples out of the Wi-Fi receiver, sent FM and SSB by
steering the radio's test tone, played arbitrary waveforms through the transmit DAC, and put
DVB-S on the 13 cm band from a C3 with a PC doing the heavy lifting.

This repo goes for the whole thing: **a complete DVB-S2 transmitter running on one ESP32-S3**.
Framing, error correction, modulation and pulse shaping all happen on the chip, and the signal
leaves through the S3's own Wi-Fi radio. No FPGA, no external DAC, no mixer, no local
oscillator. One chip and an antenna connector.

**The goal is live video:** 8PSK rate 2/3 at 5 Msym/s carries 9.68 Mb/s in about 6 MHz of the
13 cm amateur band. The first milestone is QPSK at 1 Msym/s decoded by a real receiver.

## How it works

```
payload ─▶ BB frame ─▶ BCH ─▶ LDPC ─▶ 8PSK map ─▶ PL frame ─▶ pulse shaping ─▶ DAC bank ─▶ radio
           └────────────── one core ──────────────┘          └──────── other core ────────┘
```

The S3's radio contains an undocumented test engine that plays samples from internal RAM
straight into its I/Q DAC. It loops over one bank of 16380 samples at 40 MS/s (409.5 µs per
pass) with no CPU help, while the CPU fills a second bank and switches them on time. That refill
is the budget that decides everything: at 40 MS/s a fresh bank is due every 357 µs.

Pulse shaping uses the trick that made the [RP2350 version](https://github.com/Rivercraft911/RP2350_IQ_Benchmark)
fast: every possible output of the shaping filter is computed once into a table, so producing
samples is copying table rows instead of doing filter math. At 5 Msym/s one symbol is one row of
8 samples.

## First numbers off the board

![Time to fill one DAC bank and memory bandwidth on the ESP32-S3](docs/img/membench.png)

Copying 8-sample table rows fills a bank in 179 µs, half the 357 µs deadline on one 240 MHz
core. Combining separate I and Q tables in plain C takes 401 µs and misses it, so that step
moves to the S3's 128-bit vector instructions next. PSRAM reads at 59 MB/s, far below the DAC's
160 MB/s, so samples are made in internal RAM and PSRAM only holds payload.
[Run details](results/2026-10-16-memory/README.md).

## The encoder on the board

![Encoder cycles per frame by stage on the ESP32-S3](docs/img/fecbench.png)

The first C encoder matches the Python reference on every frame the board produced. It is
written as plain bit-level loops, and that is already enough for QPSK at 1 Msym/s (27% of one
core). 8PSK at 5 Msym/s needs it 2.3 times faster. Interleaving and mapping are the biggest slice.
On the RP2350 the same kind of bit-level code, BCH and LDPC, ran 11 times faster once rewritten
with tables and whole-word operations (4.1 M to 0.38 M cycles per frame).
[Run details](results/2026-10-16-encoder/README.md).

## Status

| Step | State |
|---|---|
| Reference encoder in Python, matching gr-dtv and leansdr stage by stage | done |
| Firmware scaffold and memory benchmarks on the ESP32-S3 | done |
| DVB-S2 encoder in C on the S3, bit-exact with the reference | done, QPSK fast enough |
| Encoder fast enough for 8PSK at 5 Msym/s | next, needs 2.3x |
| DAC bank streaming, received on an SDR through attenuators | next |
| QPSK at 1 Msym/s decoded by a DVB-S2 receiver | |
| 8PSK 2/3 at 5 Msym/s with the payload arriving over SPI | |

## Build and run

Needs ESP-IDF v6.1, an ESP32-S3 with 8 MB of PSRAM, Python 3 with numpy, pyserial and matplotlib.

```sh
python3 tests/test_dvbs2.py                     # reference encoder checks, no hardware
python3 tests/test_dvbs2_8psk.py                # independent 8PSK standard-formula checks
python3 tests/test_dvbs2_c.py                   # compile with cc, compare every C stage
python3 tests/test_fecbench.py                  # seeded benchmark frames and output CRCs
cd firmware && idf.py build flash && cd ..      # after sourcing ESP-IDF
python3 host/board.py info --reset              # talk to the board
python3 host/board.py membench --out results/<date>-<topic>/runs.jsonl
python3 host/board.py fecbench --reset --timeout 120 --out results/<date>-encoder/runs.jsonl
python3 host/board.py "fecbench pie" --timeout 120 --out results/<date>-encoder/runs.jsonl  # or c
python3 tests/test_fecbench.py results/<date>-encoder/runs.jsonl
python3 host/plots.py                           # redraw the figures from results/
```

## Where things are

| Path | What |
|---|---|
| `firmware/` | ESP-IDF application |
| `host/` | Python reference encoder, board tool, figures |
| `tests/` | Bit-exact checks of the reference |
| `results/` | One folder per measurement run, with its raw records |
| `docs/` | [Prior art](docs/prior-art.md), [bench setup](docs/hardware.md), figures |

## Transmitting

Operating outside the Wi-Fi channels needs an amateur radio licence, and the firmware is meant
for licensed use in the 13 cm band. Bench tests go into a dummy load or through at least 30 dB
of attenuation into a receiver. See [docs/hardware.md](docs/hardware.md).

## Licence

Apache-2.0. Everything here is written from scratch; the projects that showed the way are
credited in [docs/prior-art.md](docs/prior-art.md).

Nothing has gone over the air yet. Coming soon to a repo near you!
