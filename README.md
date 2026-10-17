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
| QPSK 9/10, roll-off 0.20 | 33.33 Msym/s | **40 MHz** | **58.2 Mb/s** | keyed | 12% fill, encoder about 1% ([run](results/2026-10-17-moonshot/README.md)) |
| 8PSK 5/6 | 20 Msym/s | 27 MHz | **48.4 Mb/s** | keyed | 14% ([radio off](results/2026-10-17-next4/README.md); keyed with the [earlier pulse](results/2026-10-16-keyed-20m/README.md) at 4%) |
| QPSK 9/10 | 26.67 Msym/s | 36 MHz | 46.6 Mb/s | keyed | 18% ([run](results/2026-10-17-keyed-qpsk/README.md)) |
| 8PSK 2/3 | 20 Msym/s | 27 MHz | 38.7 Mb/s | keyed | 14% (radio off, current pulse) |
| QPSK 9/10 | 20 Msym/s | 27 MHz | 34.9 Mb/s | keyed | 22% ([run](results/2026-10-17-keyed-qpsk/README.md)) |
| 8PSK 5/6 | 10 Msym/s | 13.5 MHz | 24.2 Mb/s | keyed | 49% ([run](results/2026-10-16-keyed-next2/README.md)) |
| QPSK 9/10 | 10 Msym/s | 13.5 MHz | 17.5 Mb/s | radio off | 48% ([run](results/2026-10-17-cleanup/README.md)) |
| 8PSK 9/10 | 5 Msym/s | 6.75 MHz | 13.1 Mb/s | radio off | 55% |
| 8PSK 2/3, the video mode | 5 Msym/s | 6.75 MHz | 9.68 Mb/s | keyed | 57% ([run](results/2026-10-16-keyed-next2/README.md)) |
| QPSK 1/2 | 1 Msym/s | 1.35 MHz | 0.97 Mb/s | keyed | 52% |

Roll-off is 0.35 unless noted; data rates are the DVB-S2 payload of normal frames with pilots.
Every QPSK (1/2 to 9/10) and 8PSK (3/5 to 9/10) code rate is encoded at these symbol rates; the
table lists the ones measured. The chip's own samples decode bit-exact in an independent
receiver (LeanDVB) at every symbol rate here. **Not yet shown:** a receiver decoding the signal over
RF, which is the next measurement. 8PSK at 40 MHz (87.3 Mb/s) is the monster still lurking:
its tables do not fit in RAM yet.

## Under the hood

| | Best measured | |
|---|---|---|
| Encoder | 8PSK 2/3 frame in 167k cycles, QPSK 9/10 in 148k (live stream, 240 MHz core) | [run](results/2026-10-16-next2/README.md) |
| Pulse shaping | 1.78 cycles per sample from tables; a 40 MHz bank in 41k of its 47k cycles | [shaper](results/2026-10-16-shaper/README.md) |
| Signal quality of the samples | 47.0 dB MER at 1 Msym/s, 38-45 dB at the fast rates (digital, before RF) | [dry run](results/2026-10-16-dryrun/README.md) |
| Payload in | quad SPI: 46.8 Mb/s verified (one-board loopback); USB: 4.6 Mb/s | [SPI](results/2026-10-17-spi-in/README.md) |
| RF, test tone | image -35 dBc, carrier leak about -45 dBc (over the air, uncalibrated) | [first RF](results/2026-10-16-radio/README.md) |
| Station ID | 20 WPM Morse at start, end and every 9 minutes; callsign in every frame | [station ID](results/2026-10-16-station-id/README.md) |

## How it works

```
payload -> BB frame -> BCH -> LDPC -> map -> PL frame -> pulse shaping -> DAC bank -> radio
           <-------------- core 0 --------------->    <--------- core 1 --------->
```

The S3's radio hides a test engine that plays samples from internal RAM straight into its I/Q
DAC. It loops one bank of 16,380 samples while the CPU fills the other and switches them on time.
At 80 MS/s a fresh bank is due every 196.75 µs, or 47,220 cycles at 240 MHz, and that refill is
the budget that decides everything. Core 0 encodes frames continuously into a small ring; if it
ever falls behind, valid dummy frames go out instead, so the schedule never breaks.

To pulse shape with such speed and tenacity I went a similar path to my
[RP2350 version](https://github.com/Rivercraft911/RP2350_IQ_Benchmark): every possible output of
the shaping filter is computed once into a table, so producing samples is copying table rows
instead of doing filter math. At 40 MHz the trick goes one further: QPSK's I and Q are two BPSK
streams, so each four-sample vector comes from small sign-indexed tables and one OR.
[Fill](docs/bank-fill.md), [encoder](docs/encoder.md), [radio and bank geometry](docs/bank-geometry.md),
[40 MHz](docs/moonshot.md).

## Build and check

Needs ESP-IDF v6.1, ESP32-S3 with 8 MB PSRAM, Python 3 with numpy, scipy,
pyserial and matplotlib. Independent decode also needs a local LeanDVB build.

```sh
python3 tests/test_dvbs2.py            # Python reference and standard checks
python3 tests/test_dvbs2_c.py          # every C encoder stage against Python
python3 tests/test_fecbench.py         # actual benchmark command, all 140 CRCs
python3 tests/test_stream.py           # actual stream, every DAC word and 58 golden CRCs
. ~/.espressif/tools/activate_idf_v6.1.sh >/dev/null 2>&1
(cd firmware && idf.py build)
host/check.sh --plan                  # pass criteria only; opens no devices
```

CI runs the remaining host tests and builds firmware plus the shaper, encoder
and radio QEMU harnesses. In a sandbox that denies the unused component
manager's process discovery, use `IDF_COMPONENT_MANAGER=0 idf.py build`.
No managed dependencies are required.

The coordinator owns flashing and board checks. After building/flashing the
committed revision, with N0CALL already stored, choose a Python with the host
dependencies (the example uses the local macOS installation):

```sh
PYTHON=/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 \
  RX=/path/to/leandvb host/check.sh notes/cleanup-board
# Add --all-crcs for an exhaustive board CRC/decode sweep (much slower).
```

The slim check takes about five minutes, depending on USB and decode speed.
It prints gates first, keeps going after failures, records each outcome and
ends with a summary. It checks all golden waveforms on the host, selected
16-frame board dumps plus regressions, LeanDVB, four key 30 s paces and four
10 s regressions covering every modulation/rate path, and Wi-Fi startup heap. Timing ceilings are 102% of recorded main values;
8PSK 9/10 at 5 Msym/s must retain at least 22,351 B with Wi-Fi up. It never
flashes, changes NVS or keys the radio. [Exact gates](docs/bank-geometry.md#evidence-and-acceptance).

`host/check.sh --moonshot` adds the 40 MHz checks (`33m333`: QPSK at 100/3 Msym/s,
roll-off 0.20) with 15% reserve gates; today that mode passes its deadlines but not the
reserve. [Design and gates](docs/moonshot.md).

## Console and host tools

QPSK defaults to 1 Msym/s. Both modulations accept `5m`, `10m`, `20m`;
`26m667` and `33m333` (roll-off 0.20, 40 MHz) are QPSK only. Rate tokens precede the frame count or duration.
Dumps emit little-endian int16 I/Q; full scale for analysis is 512.

```sh
python3 host/board.py info --reset
python3 host/board.py membench                      # shipped shapers and C oracles
python3 host/board.py fecbench --timeout 120         # shipped encoder; or "fecbench plain"
python3 host/board.py "dvbdump 4 40" --save qpsk.iq   # legacy 1 Msym/s, 4 MS/s dump
python3 host/board.py "dvbdump 15 20m 16 128 1" --save fast.iq
python3 host/onair.py fast.iq --rate 80000000 --symbol-rate 20000000 \
  --modcod 15 --scale 512 --stream 16 --callsign N0CALL --shift 0 --receiver /path/to/leandvb
python3 host/board.py "dvbpace 11 26m667 30" --timeout 120
python3 host/board.py "wifimem 17 5m"                 # Wi-Fi start/stop only
python3 host/plots.py                               # historical figures from results/
```

Use the board's actual stored callsign when decoding; golden CRCs use N0CALL.
`dvbpace` exercises the live encoder and bank deadlines without radio playback.
`wifimem` measures startup next to the selected transmitter allocations.
`host/stream_profile.py` summarizes saved cycle/cache records.

## Files

| Path | Purpose |
|---|---|
| `firmware/main/` | Console, transmitter lifecycle, frame ring, shaping kernels and two benchmarks |
| `firmware/components/` | DVB-S2 encoder, S3 radio/bank scheduler, station ID and storage |
| `host/dvbs2.py`, `host/shaper.py`, `host/qpsk_fill.py` | Independent encoder and waveform models, table generation |
| `host/check.sh`, `host/board.py`, `host/onair.py` | Coordinator gates, board protocol, decode/MER analysis |
| `host/bench.py`, `host/sdr.py`, `host/capture.py` | Authorized RF capture and offline diagnostics |
| `tests/` | Host checks, golden digests/CRCs and peripheral-free QEMU harnesses |
| `results/` | Immutable measured evidence, raw records and firmware versions |
| `docs/` | [Story](docs/story.md), [fill](docs/bank-fill.md), [encoder](docs/encoder.md), [radio](docs/bank-geometry.md), [bench setup](docs/hardware.md), [prior art](docs/prior-art.md), [file link](docs/file-link.md), generated figures |

`host/filelink.py` remains a tested host-only erasure-coded file transport.
Historical experiment kernels and merge diaries live in git history; results
remain unchanged. README/story figures are generated by `host/plots.py`.

## Transmitting

Only a licensed operator should key the radio on an authorized amateur
frequency, into a dummy load or through at least 30 dB attenuation into a
receiver. Set the assigned callsign with `callsign <CALL>` first. `tx` and `dvb`
refuse without it. The keyed syntax is
`dvb <MHz> <MODCOD> [5m|10m|20m|26m667|33m333] <seconds> [gain] [peak]`.

Runs include 20 WPM Morse at the start/end and periodic ID every nine minutes;
requested RF duration includes both IDs and is limited to 60 s. Payload frames
also carry the callsign. `iddump CALL` and `host/morse.py` check ID samples
without RF. [Setup, limitations and capture commands](docs/hardware.md).

## Licence

Apache-2.0. Independently written; findings from other projects are credited in
[prior-art.md](docs/prior-art.md).

A receiver decoding it over the air is next. Coming soon to a repo near you!
