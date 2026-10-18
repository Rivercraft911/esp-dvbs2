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
ever built for. [How it got here](docs/story.md), wrong turns included.

## What it can do

The best measured configurations, fastest first. **Keyed** means the S3 really transmitted for
30 s (into a shielded box) and met every bank deadline; **radio off** means the same encoder and
bank schedule ran in real time with the DAC output going nowhere. Slack is how much of each bank
period the busiest core had left over.

| Mode | Symbol rate | Bandwidth | **Data rate** | Tested | Slack |
|---|---:|---:|---:|---|---|
| QPSK 9/10, roll-off 0.20 | 33.33 Msym/s | **40 MHz** | **58.2 Mb/s** | keyed | 12% fill, encoder about 1% ([run](docs/measurements.md)) |
| 8PSK 5/6 | 20 Msym/s | 27 MHz | **48.4 Mb/s** | keyed | 14% ([radio off](docs/measurements.md); keyed with the [earlier pulse](docs/measurements.md) at 4%) |
| QPSK 9/10 | 26.67 Msym/s | 36 MHz | 46.6 Mb/s | keyed | 18% ([run](docs/measurements.md)) |
| 8PSK 2/3 | 20 Msym/s | 27 MHz | 38.7 Mb/s | keyed | 14% (radio off, current pulse) |
| QPSK 9/10 | 20 Msym/s | 27 MHz | 34.9 Mb/s | keyed | 22% ([run](docs/measurements.md)) |
| 8PSK 5/6 | 10 Msym/s | 13.5 MHz | 24.2 Mb/s | keyed | 49% ([run](docs/measurements.md)) |
| QPSK 9/10 | 10 Msym/s | 13.5 MHz | 17.5 Mb/s | radio off | 48% ([run](docs/measurements.md)) |
| 8PSK 9/10 | 5 Msym/s | 6.75 MHz | 13.1 Mb/s | radio off | 55% |
| 8PSK 2/3, the video mode | 5 Msym/s | 6.75 MHz | 9.68 Mb/s | keyed | 57% ([run](docs/measurements.md)) |
| QPSK 1/2 | 1 Msym/s | 1.35 MHz | 0.97 Mb/s | keyed | 52% |

Roll-off is 0.35 unless noted. Bandwidth and data rates are calculated from the DVB-S2
normal-frame format with pilots; data rates exclude time spent sending station ID.
QPSK code rates 1/2 to 9/10 and 8PSK 3/5 to 9/10 are implemented. The table lists measured
configurations. The chip's samples decode bit-exact in an independent receiver, LeanDVB.
**Not yet shown:** a receiver decoding the signal over RF. The current six-symbol 8PSK
20 Msym/s pulse has radio-off evidence; the keyed result used an earlier nine-symbol pulse.
[Measurements, raw records and limits](docs/measurements.md).

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

## Build and check

Needs an ESP32-S3 with 8 MB octal PSRAM, 16 MB flash, native USB and ESP-IDF v6.1.
The measured board runs both cores at 240 MHz. Python 3.12 or later needs numpy, scipy
and pyserial; independent decoding uses a separate LeanDVB build.

```sh
python3 -m pip install numpy scipy pyserial
(. ~/.espressif/tools/activate_idf_v6.1.sh >/dev/null 2>&1 && cd firmware && idf.py build)
python3 tests/test_dvbs2.py
python3 tests/test_dvbs2_8psk.py
python3 tests/test_dvbs2_c.py
python3 tests/test_stream.py
python3 tests/test_rational.py
python3 tests/test_self_check.py
python3 examples/self-check/check.py --plan
```

CI runs those correctness checks, the SPI checks, and builds the transmitter. The host
stream tests exercise the real C lifecycle and compare every sample and pinned CRC.
They cannot establish board timing. If a sandbox blocks ESP-IDF's unused component manager,
use `IDF_COMPONENT_MANAGER=0 idf.py build`; no managed dependencies are needed.

After flashing the committed revision, run the [radio-off self-check](examples/self-check/README.md).
It dumps six waveforms, decodes them with LeanDVB, runs a paced representative of every
modulation/rate path, and checks Wi-Fi startup memory. It takes about five minutes,
prints pass criteria first, and keeps going after failures. It never flashes or transmits.

## Console

Use `python3 host/board.py info --reset` to reset and query the chip through native USB.
On macOS, the host tool avoids the native port's RTS reset, which can enter the bootloader.
[Ports and setup](docs/transmitting.md).

| Command | Purpose |
|---|---|
| `info` | Firmware revision, clocks, memory and stored callsign |
| `callsign [CALL\|clear]` | Read, set or clear the station ID |
| `dvbdump MODCOD [rate] frames [peak] [decimation] [stall_ms]` | Samples over USB, radio off |
| `dvbpace MODCOD [rate] seconds [peak] [stall_ms]` | Live encoder and bank deadlines, no playback |
| `wifimem MODCOD [rate]` | Radio-off Wi-Fi startup and heap check, also used by SPI integration |
| `iddump [CALL] [40\|80]` | Morse ID samples at 10 kS/s, radio off |
| `tx MHz signal seconds [gain] [40\|80]` | Transmit a test tone; see setup first |
| `dvb MHz MODCOD [rate] seconds [gain] [peak]` | Transmit DVB-S2; see setup first |

MODCOD selects modulation and code rate: QPSK 4..11, 8PSK 12..17. The
[rate table](docs/how-it-works.md#rates) gives the exact choices. QPSK defaults to 1 Msym/s.
Both modulations accept `5m`, `10m`, `20m`; `26m667` also has an [experimental 8PSK path](docs/lofty.md); `33m333` is QPSK only.
Dumps are little-endian int16 I/Q with full scale 512. Golden CRCs use peak 128 and N0CALL.

```sh
python3 host/board.py "dvbdump 15 20m 16 128 1" --save fast.iq
python3 host/decode.py fast.iq --rate 80000000 --symbol-rate 20000000 \
  --modcod 15 --stream 16 --callsign N0CALL --receiver /path/to/leandvb
python3 host/board.py "dvbpace 11 26m667 10" --timeout 120
```

## Files

| Path | Purpose |
|---|---|
| `firmware/` | Console, transmitter, encoder, radio, station ID and SPI input |
| `host/board.py`, `host/decode.py`, `host/morse.py` | Board protocol, independent decode and ID checking |
| `host/dvbs2.py`, `host/dvbs2_tables.py` | Independent Python oracle and standard tables |
| `host/plots.py` | Regenerate the measurement figures from the retained raw records |
| `examples/self-check/` | Radio-off example and board regression check |
| [`examples/live-video/`](examples/live-video/README.md) | UHD/LeanDVB video receiver and RF-free TS-to-IQ dry run |
| `tests/` | Standard vectors, C/Python agreement and stream CRCs; table generators in `reference/` |
| `docs/` | [How it works](docs/how-it-works.md), [transmitting](docs/transmitting.md), [story](docs/story.md), [measurements](docs/measurements.md), [credits](docs/prior-art.md) |

[SPI input](docs/spi-input.md), its host tools and its tests are retained for the separate
PigeonVision input work. The full development history, experiments and raw results live in
[archive/dev-2026-10-17](https://github.com/Rivercraft911/esp-dvbs2/tree/archive/dev-2026-10-17).

## Transmitting

Only a licensed operator should key the radio on an authorized amateur frequency, in a
shielded setup or into a dummy load, or through at least 30 dB attenuation into a receiver.
Set your assigned callsign first. The firmware sends 20 WPM Morse before and after a run;
payload frames also carry the callsign. [Setup and limitations](docs/transmitting.md).

## Licence

Apache-2.0. Independently written; findings from other projects are credited in
[prior-art.md](docs/prior-art.md).

A receiver decoding it over the air is next. Coming soon to a repo near you!
