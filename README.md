# esp-dvbs2

A complete DVB-S2 transmitter on one ESP32-S3. Framing, error correction,
modulation and pulse shaping run on the chip; samples leave through its own
Wi-Fi radio. No PC encoder or external DAC.

Main has keyed **QPSK 9/10 at 80/3 Msym/s (46.6 Mb/s)** and **8PSK 5/6 at
20 Msym/s (48.4 Mb/s)** for 30 s each in a shielded box, with every bank deadline
met. Those are measured playback results. A receiver has not yet decoded the
signal over RF. [QPSK run](results/2026-10-17-keyed-qpsk/README.md),
[8PSK run](results/2026-10-16-keyed-20m/README.md).

The goal is live video. 8PSK 2/3 at 5 Msym/s carries 9.68 Mb/s before application
framing, with a nominal 6.75 MHz occupied bandwidth at roll-off 0.35.
[How it got here](docs/story.md) follows the measurements and failed attempts.

## What runs

| Modulation | MODCOD | Symbol rates | Evidence |
|---|---|---|---|
| QPSK | 4..11, rates 1/2 through 9/10 | 1, 5, 10, 20, 80/3 Msym/s | [Host/board CRCs and independent decode](results/2026-10-17-qpsk-goal/README.md) |
| 8PSK | 12..17, rates 3/5 through 9/10 | 5, 10, 20 Msym/s | [5/10 Msym/s](results/2026-10-16-8psk-board/README.md), [20 Msym/s short pulse](results/2026-10-17-next4/README.md) |

The encoder supports every listed MODCOD. Board and receiver checks cover
representative rates, not every combination over RF. The Python reference also
has 16APSK/32APSK; firmware does not.

Cleanup selects the fastest measured 20 Msym/s kernel per modulation: QPSK's
four-symbol block kernel and 8PSK's six-symbol pulse. These previously used
`20mq4` and `20ms6`; both are now `20m`. The duplicate `10m80` token is removed.
All other waveforms retain their CRCs. The new defaults match the old opt-in
kernels bit for bit; this branch still needs the coordinator's board checks.
The six-symbol 8PSK waveform has radio-off evidence, not keyed qualification.
[Waveforms, CRCs and memory](docs/bank-fill.md).

Quad SPI input verified 46.768 Mb/s in standalone loopback. Paced input and Wi-Fi
reserve fixes await another board run; external senders remain unqualified.
[Wire format, XIAO/DevKit pins, sender, budget and radio-off gates](docs/spi-input.md).

## How it works

```
payload -> BB frame -> BCH -> LDPC -> map -> PL frame -> pulse shaping -> DAC bank -> radio
           <-------------- core 0 --------------->    <--------- core 1 --------->
```

The radio's test engine loops over 16,380 complex samples in internal SRAM.
Core 1 fills the other bank and switches it on time. At 80 MS/s a fresh bank
is due every 196.75 us, or 47,220 cycles at 240 MHz. Core 0 continuously encodes
frames into a small ring. Underruns insert valid dummy frames without skipping
payload. Lookup tables replace per-sample filter multiplication; finished
overlap samples are reused.
[Fill](docs/bank-fill.md), [encoder](docs/encoder.md),
[radio and bank geometry](docs/bank-geometry.md).

USB payload input reached 4.6 Mb/s. The test-tone image was -35 dBc and carrier
leakage about -45 dBc.
The RF passband, DAC continuity and receiver decode still need cabled
measurements. Host models, instruction models and QEMU are correctness or
planning tools, not silicon timing measurements.
[USB](results/2026-10-16-usb/README.md), [RF](results/2026-10-16-radio/README.md).

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

The experimental `33m333` token runs QPSK/8PSK at exactly 100/3 Msym/s with
roll-off 0.20; 8PSK also accepts `26m667`. Host-generated C dumps decode bit-exact
in LeanDVB. The split QPSK fill fails its first silicon pace; its measured
Wi-Fi heap passes. Both new 8PSK modes fail allocation. RF remains unmeasured.
[System schedule and coordinator gates](docs/moonshot.md).
`host/check.sh --moonshot --plan` prints the 15% reserve and 20,000 B heap gates.

## Console and host tools

QPSK defaults to 1 Msym/s. Both modulations accept `5m`, `10m`, `20m`;
`26m667` is QPSK only. Rate tokens precede the frame count or duration.
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
`dvb <MHz> <MODCOD> [5m|10m|20m|26m667] <seconds> [gain] [peak]`.

Runs include 20 WPM Morse at the start/end and periodic ID every nine minutes;
requested RF duration includes both IDs and is limited to 60 s. Payload frames
also carry the callsign. `iddump CALL` and `host/morse.py` check ID samples
without RF. [Setup, limitations and capture commands](docs/hardware.md).

## Licence

Apache-2.0. Independently written; findings from other projects are credited in
[prior-art.md](docs/prior-art.md).
