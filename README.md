# esp-dvbs2

What started as but one hack on the ESP32-C3 has snowballed into a plethora of ESP32 software
defined radios. People have pulled raw I/Q samples out of the Wi-Fi receiver, sent FM and SSB by
steering the radio's test tone, played arbitrary waveforms through the transmit DAC, and put
DVB-S on the 13 cm band from a C3 with a PC doing the encoding.

Now... this repo goes for the whole thing: **a complete DVB-S2 transmitter running on one ESP32-S3**.
Framing, error correction, modulation and pulse shaping all happen on the chip, and the signal
leaves through the S3's own Wi-Fi radio.

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

To pulse shape with such speed and tenacity I went a similar path to my [RP2350 version](https://github.com/Rivercraft911/RP2350_IQ_Benchmark):
every possible output of the shaping filter is computed once into a table, so producing
samples is copying table rows instead of doing filter math. At 5 Msym/s one symbol is one row of
8 samples. There are a bunch of ways you can play with this idea as you try more complex waveforms.

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

The transmitter now uses the fast complete-frame API. Its 8PSK 2/3 context takes 14,736 bytes
of internal RAM; shared tables stay in flash and the workspace uses cached PSRAM. Host tests
and QEMU agree bit for bit. The instruction pipeline model predicts 179k cycles per GCS frame
and 205k with TS input after warmup. Board timing and shared-cache contention are still unverified.

## Pulse shaping at 1.78 cycles per sample

![Time to shape one DAC bank with plain C and with the PIE vector unit](docs/img/shaper.png)

Real 8PSK samples, a 9-symbol root-raised-cosine pulse, go into a DAC bank at 1.78 cycles per
sample using the S3's 128-bit vector instructions. Each sample is the sum of three table words,
so the whole 8PSK table fits in 64 KiB. That is 34% of one core at 40 MS/s, and 68% at 80 MS/s,
the rate that would allow 20 Msym/s. [Run details](results/2026-10-16-shaper/README.md).

## Status

| Step | State |
|---|---|
| Reference encoder in Python, matching gr-dtv and leansdr stage by stage | done, including [normal 16APSK/32APSK](results/2026-10-16-apsk-decode/README.md) |
| Firmware scaffold and memory benchmarks on the ESP32-S3 | done |
| DVB-S2 encoder in C on the S3, bit-exact with the reference | done |
| Offline IQ decoded by an independent DVB-S2 receiver | done, [40 PSK cases](results/2026-10-16-decode/README.md) and [56 APSK cases](results/2026-10-16-apsk-decode/README.md), whole payloads match |
| Encoder fast enough for 8PSK at 5 Msym/s (tables, whole words, PIE) | done, [214k cycles per 8PSK 2/3 frame](results/2026-10-16-fec-limit/README.md), 20% of a core |
| Pulse shaping fast enough for 8PSK at 5 Msym/s (PIE, 1.78 cycles/sample) | done |
| DAC bank streaming received on an SDR | done, [first RF](results/2026-10-16-radio/README.md) |
| Whole chain on the chip, bank samples decoded by LeanDVB (radio off) | done, [dry run](results/2026-10-16-dryrun/README.md), 47 dB MER |
| Continuous encoding on one core while the other fills banks (radio off) | done, [stream](results/2026-10-16-stream/README.md), every deadline met |
| 8PSK in the transmitter, radio off | 5 Msym/s bit-exact with 7% fill slack; 10 Msym/s bit-exact but misses the fill deadline ([run](results/2026-10-16-8psk-board/README.md)) |
| QPSK at 1 Msym/s decoded by a DVB-S2 receiver | |
| 8PSK 2/3 at 5 Msym/s with the payload arriving over SPI | |

8PSK MODCOD 12..17 is now wired into the continuous transmitter at 5 and 10 Msym/s.
Use an explicit rate token: `dvbdump 13 5m 40` or `dvbpace 15 10m 60`.
QPSK commands keep their syntax. The pulse and BBHEADER both use roll-off 0.35.
Underruns insert standard 3,330-symbol dummy PLFRAMEs. On the board, radio off, all four
8PSK cases decode bit-exact; 5 Msym/s holds its deadlines with thin margin, and 10 Msym/s
does not fit yet ([board run](results/2026-10-16-8psk-board/README.md)). RF is still to come.

## Build and run

Needs ESP-IDF v6.1, an ESP32-S3 with 8 MB of PSRAM, Python 3 with numpy, pyserial and matplotlib.
The host IQ tools also need scipy. Independent loopback needs a separate local LeanDVB build.

```sh
python3 tests/test_dvbs2.py                     # reference encoder checks, no hardware
python3 tests/test_dvbs2_8psk.py                # independent 8PSK standard-formula checks
python3 tests/test_dvbs2_apsk.py                # normal 16APSK/32APSK formulas and stage digests
python3 tests/test_dvbs2_c.py                   # compile with cc, compare every C stage
python3 tests/test_fecbench.py                  # seeded benchmark frames and output CRCs
python3 tests/test_iq.py                        # RRC waveform and capture formats, no hardware
python3 tests/test_decode.py                    # receiver output parsing and failure checks
python3 tests/test_ring.py                      # encoder-to-fill frame ring against the bank schedule
cd firmware && idf.py build flash && cd ..      # after sourcing ESP-IDF
python3 host/board.py info --reset              # talk to the board
python3 host/board.py membench --out results/<date>-<topic>/runs.jsonl
python3 host/board.py fecbench --reset --timeout 120 --out results/<date>-encoder/runs.jsonl
python3 host/board.py "fecbench pie" --timeout 120 --out results/<date>-encoder/runs.jsonl  # or c
python3 tests/test_fecbench.py results/<date>-encoder/runs.jsonl
python3 host/board.py "dvbdump 4 40" --save dump.iq   # the transmit samples over USB, radio off
python3 host/onair.py dump.iq --rate 4000000 --scale 512 --stream 40   # LeanDVB decode, payload check
python3 host/board.py "dvbpace 4 60" --timeout 120  # encoder and timed bank fills, radio off
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

The first test tones have left the board. DVB-S2 over the air is next. Coming soon to a repo near you!
