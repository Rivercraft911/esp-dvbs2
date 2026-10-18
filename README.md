# esp-dvbs2

What started as but one hack on the ESP32-C3 has snowballed into a plethora of ESP32 software
defined radios. People have pulled raw I/Q samples out of the Wi-Fi receiver, sent FM and SSB by
steering the radio's test tone, played arbitrary waveforms through the transmit DAC, and put
DVB-S on the 13 cm band from a C3 with a PC doing the encoding.

Now... this repo goes for the whole thing: **a complete DVB-S2 transmitter running on one ESP32-S3**.
Framing, error correction, modulation and pulse shaping all happen on the chip, and the signal
leaves through the S3's own Wi-Fi radio. No PC doing the encoding, no external DAC, no FPGA.

It started with the goal of live video, 8PSK 2/3 at 5 Msym/s, 9.68 Mb/s. Then it kept going:
the same $5 chip now pushes **58.2 Mb/s in 40 MHz**, the widest channel its Wi-Fi radio was
ever built for. Read [How it got here](docs/story.md) to learn about the tumultuous journey.

> **[Placeholder: spectrum of the 40 MHz signal and its constellation, from the chip's own samples]**

## Live video

> **[Placeholder: demo clip, camera feeds received off the air and played on a laptop]**

```
CM5 + cameras --SPI, 9 Mb/s TS--> ESP32-S3 --DVB-S2--> SDR --> LeanDVB --> video player
```

A Raspberry Pi CM5 encodes its cameras to H.264 and sends a 9 Mb/s MPEG transport stream over
SPI. The S3 frames, codes and shapes it as QPSK 3/4 at 10 Msym/s, 14.5 Mb/s in 13.5 MHz, and
transmits it. An SDR and LeanDVB turn it back into a transport stream for any video player.
The whole chain already runs end to end without RF in the
[live-video example](examples/live-video/README.md); the over-the-air demo is next.

## What it can do

The best measured configurations, fastest first. **Keyed** means the S3 really transmitted for
30 s and met every bank deadline; **radio off** means the same encoder and bank schedule ran in
real time with the DAC output going nowhere.

| Mode | Symbol rate | Bandwidth | **Data rate** | Tested |
|---|---:|---:|---:|---|
| QPSK 9/10, roll-off 0.20 | 33.33 Msym/s | **40 MHz** | **58.2 Mb/s** | keyed |
| 8PSK 5/6 | 20 Msym/s | 27 MHz | **48.4 Mb/s** | keyed* |
| QPSK 9/10 | 26.67 Msym/s | 36 MHz | 46.6 Mb/s | keyed |
| 8PSK 2/3 | 20 Msym/s | 27 MHz | 38.7 Mb/s | keyed* |
| QPSK 9/10 | 20 Msym/s | 27 MHz | 34.9 Mb/s | keyed |
| 8PSK 5/6 | 10 Msym/s | 13.5 MHz | 24.2 Mb/s | keyed |
| QPSK 9/10 | 10 Msym/s | 13.5 MHz | 17.5 Mb/s | radio off |
| 8PSK 9/10 | 5 Msym/s | 6.75 MHz | 13.1 Mb/s | radio off |
| 8PSK 2/3, the video mode | 5 Msym/s | 6.75 MHz | 9.68 Mb/s | keyed |
| QPSK 1/2 | 1 Msym/s | 1.35 MHz | 0.97 Mb/s | keyed |

Roll-off is 0.35 unless noted. Data rates follow from the DVB-S2 normal frame with pilots and
exclude station-ID time. Every mode's samples decode bit-exact in LeanDVB, an independent
receiver; a receiver decoding the signal over the air is not shown yet.
\*Keyed with an earlier, longer pulse; the current pulse has radio-off evidence.
Margins, raw records and limits: [measurements](docs/measurements.md). The 40 MHz mode keeps
a 15% reserve on both cores while keyed.

![Calculated payload rates from selected historical keyed records](docs/img/payload.png)

## Quick start

You need an ESP32-S3 with 8 MB octal PSRAM and 16 MB flash (an N16R8 module), ESP-IDF v6.1,
and Python 3.12+ with numpy, scipy and pyserial. Nothing below turns the radio on.

```sh
. ~/.espressif/tools/activate_idf_v6.1.sh
(cd firmware && idf.py build flash)
python3 host/board.py info --reset
python3 host/board.py "callsign N0CALL"      # placeholder for radio-off tests only
```

Dump 16 frames of 8PSK 5/6 at 20 Msym/s over USB, exactly as the chip would transmit them,
and decode them with LeanDVB. Every payload byte should come back.

```sh
python3 examples/self-check/build_receiver.py   # builds LeanDVB under notes/
python3 host/board.py "dvbdump 15 20m 16 128 1" --save fast.iq
python3 host/decode.py fast.iq --rate 80000000 --symbol-rate 20000000 \
  --modcod 15 --stream 16 --receiver notes/leansdr/src/apps/leandvb
```

The [self-check](examples/self-check/README.md) runs the full radio-off regression in about
five minutes: six dumps and decodes, a real-time pace of every modulation and rate path, and a
Wi-Fi memory check. All console commands are in [transmitting](docs/transmitting.md#console).

## Send it your data

The S3 takes its payload over SPI. The sender clocks fixed 1,332-byte messages: a 12-byte
header (`PV`, version, type, sequence, length), up to seven 188-byte MPEG-TS packets and a
CRC-32. A READY pin tells the sender when the S3 has room. Single-line SPI at 20 MHz carries
about 19 Mb/s; a four-line mode measured 46.6 Mb/s. Payload is MPEG-TS for video or any
bytes as a generic stream. The S3 does the rest: framing, coding, filler frames when the
input runs dry, and transmission. Select it with `spiinput on` before a run; the
[reference sender](examples/pi-sender/README.md) streams a file from a Linux board.
[Message format, pins and commands](docs/spi-input.md).

## How it works

```
payload -> BB frame -> BCH -> LDPC -> map -> PL frame -> pulse shaping -> DAC bank -> radio
           <-------------- core 0 --------------->    <--------- core 1 --------->
```

The S3's radio hides a test engine that plays samples from internal RAM straight into its I/Q
DAC. It loops one bank of 16,380 samples while the CPU fills the other and switches them on time.
At 80 MS/s a fresh bank is due every 196.75 µs, or 47,220 cycles at 240 MHz.
Core 0 encodes frames into a small ring; core 1 shapes them into samples. At 40 MHz, core 0
instead publishes sign histories into a queue, which core 1 renders directly.

To pulse shape with such speed and tenacity I went a similar path to my
[RP2350 version](https://github.com/Rivercraft911/RP2350_IQ_Benchmark): every possible output of
the shaping filter is computed once into a table, so producing samples is copying table rows
instead of doing filter math. At 40 MHz the trick goes one further: QPSK's I and Q are two BPSK
streams, so each four-sample vector comes from small sign-indexed tables and one OR.
[The encoder, fill and bank schedule](docs/how-it-works.md).

## Transmitting

Only a licensed operator should key the radio on an authorized amateur frequency, in a
shielded setup or into a dummy load, or through at least 30 dB attenuation into a receiver.
Set your assigned callsign first. The firmware sends 20 WPM Morse before and after a run;
payload frames also carry the callsign. [Setup and limitations](docs/transmitting.md).

## More

| Path | What's there |
|---|---|
| `firmware/` | Console, transmitter, encoder, radio, station ID and SPI input |
| `host/` | Board protocol, independent decode, Python reference encoder, figures |
| `examples/` | [Self-check](examples/self-check/README.md), [live video](examples/live-video/README.md), [SPI sender](examples/pi-sender/README.md) |
| `tests/` | Standard vectors, C/Python agreement and stream CRCs ([running them](docs/how-it-works.md#checking-changes)) |
| `docs/` | [How it works](docs/how-it-works.md), [transmitting](docs/transmitting.md), [SPI input](docs/spi-input.md), [measurements](docs/measurements.md), [story](docs/story.md), [credits](docs/prior-art.md) |

The full development history, experiments and raw results live in
[archive/dev-2026-10-17](https://github.com/Rivercraft911/esp-dvbs2/tree/archive/dev-2026-10-17).

## Licence

Apache-2.0. Independently written; findings from other projects are credited in
[prior-art.md](docs/prior-art.md).

A receiver decoding it over the air is next. Coming soon to a repo near you!
