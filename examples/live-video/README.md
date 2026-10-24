# Live video over DVB-S2

Send a **9 Mb/s MPEG-TS** stream over SPI, then receive it with LeanDVB.
Use **8PSK 2/3 (MODCOD 13), 5 Msym/s, normal frames, pilots and .35 roll-off**:
9.68 Mb/s TS capacity in 6.75 MHz. The [Pi sender](../pi-sender/README.md) accepts
files or stdin; an existing PV-SPI v1 source can connect directly.

A CM5 sent a 9 Mb/s mux of two camera streams over 16 MHz SPI to a XIAO S3, and a B200
behind 40 dB of attenuation received it with no TS continuity gaps in 111 s
([records](../../results/2026-10-19-live-rf/README.md)). That run applied a per-board IQ/DC
correction that has since been cut from the firmware.

Earlier, in a closed box on 2026-10-18, S3 → E200 with Pluto firmware → LeanDVB
recovered 47,672 packets as an exact contiguous source slice, 99.6% of the file,
with zero continuity or PSI CRC errors. That test used USB input at about 2.8 Mb/s
and a mostly null-padded MPEG-2 fixture. A 2.4 Mb/s H.264 USB fixture also passed
live reception and playback. Neither E200 run left a record in `results/`.
USB intake, 2.59 to 2.65 Mb/s into a keyed transmitter
([TS link](../../results/2026-10-23-ts-link/README.md)), preserves a finite file but cannot
carry the live 9 Mb/s mux.

## ESP32-S3 to ESP32-S3

<p align="center"><img src="bench.jpg" alt="The bench: the transmitting S3 beside the 360 camera, coax and a 30 dB attenuator to the receiving S3 and its W5500, the link dashboard on both monitors and the received video on the laptop" width="600"></p>

A second S3 can take the B200's place. On 2026-10-23 the CM5 sent the two-camera mux over SPI
to the transmitting XIAO, which sent 8PSK 3/4 short frames at 5 Msym/s (10.39 Mb/s) at
2420 MHz through coax and 30 dB of attenuation. The receiving XIAO sent the TS to the laptop
through a W5500 Ethernet module at 10 Mb/s. In a 540 s run all 402,607 frames decoded, the
egress dropped nothing, and 0.09% of datagrams were lost between the W5500 and the laptop
([records](../../results/2026-10-23-eth-full-quality/README.md)). The module drops its link at
100 Mb/s on the XIAO's 3V3 supply, so the link is forced to 10 Mb/s
([bring-up](../../results/2026-10-23-eth-bringup/README.md)).

Wire the W5500 module to the receiver: SCLK to GPIO9, MOSI to GPIO43, MISO to GPIO44, CS to
GPIO3, and 3V3 and GND (`eth pins` changes the GPIOs). Then on the receiver, after each reset
(it keeps these settings in RAM), and the run:

```text
eth phy 10f
eth ip 192.168.1.3/24
eth dest 192.168.1.100:1234
rx 2420 32 575 14 5m ts nonull eth
```

Transmitter, with the source started after the opening station ID:

```text
spipins xiao
spiinput on ts
dvb 2420 14 5m 540 0 128 --short
```

The laptop (192.168.1.100) played both cameras with PigeonVision's `pv view` on UDP 1234;
`host/udp_ts.py --port 1234 --save received.ts` records it without a player.

For the dashboard, run `python3 host/monitor.py` and open http://127.0.0.1:8770/. The receiver's
monitor stream arrives on UDP 1235 by default, and running the transmitter's `dvb` through
`python3 host/board.py "dvb ..." --port TX_PORT --monitor 127.0.0.1:1235` adds its status lines.
`--demo` fills the page with synthetic data, no hardware needed. The
[live demo](../../results/2026-10-23-live-demo/README.md) ran this way for 2.4 hours.

## Prepare

Host Python needs numpy/scipy. Build the separate receiver with the
[pinned LeanDVB builder](../self-check/README.md#prepare):

```sh
python3 examples/self-check/build_receiver.py
export RX="$PWD/notes/leansdr/src/apps/leandvb"
```

For an RF-free model check of the demo mode:

```sh
python3 examples/live-video/dry_run.py notes/video-dry --receiver "$RX" --modcod 13
python3 examples/live-video/check_ts.py notes/video-dry/received.ts \
  --reference notes/video-dry/source.ts
```

This generates one second of MPEG-2 video in a 9 Mb/s TS, encodes 10 MS/s IQ,
and decodes it without devices. It requires at least 90% recovery as an exact
contiguous slice; acquisition/EOF trimming is allowed, interior loss is not.
Use a fresh output directory. `--ts FILE` substitutes a complete TS source.
The default `--modcod 7` checks QPSK 3/4 at 10 Msym/s instead.

## SPI source

On Linux, enable SPI0 and install `spidev` and `gpiod>=2,<3`. Find READY's
GPIO chip with `gpioinfo`; `/dev/gpiochip4` below is an example. Connect SCK,
CS, MOSI, READY and GND using the [pin table](../../docs/spi-input.md#pins).
Select the matching S3 preset, `spipins devkit` or `spipins xiao`.

Make 35 seconds of test video, then send it after the S3 run starts:

```sh
python3 examples/live-video/make_ts.py demo.ts --frames 875
python3 examples/pi-sender/send.py demo.ts --mbps 9 --gpiochip /dev/gpiochip4
```

The sender uses mode 0 at 20 MHz: one raw 1,332-byte message per CS, with READY
flow control. The ideal payload ceiling is 19.4 Mb/s before software overhead.
For ffmpeg, pipe a 9 Mb/s MPEG-TS into `send.py -`; see the
[sender example](../pi-sender/README.md). Native PV-SPI camera software needs
no Python bridge. Dummy DVB-S2 frames are expected when source data runs out.

## E200 with Pluto firmware

Use gigabit Ethernet, an explicit libiio URI and a Python with pylibiio.
Set `IIO_PYTHON`, `PORT` (native S3 USB), and authorized `FREQ_MHZ`/`FREQ_HZ`.
Follow [contained RF and station identification](../../docs/transmitting.md).
The E200 stays receive-only: the capture helper powers down TX LO, sets TX
gain to -89.75 dB and DDS outputs to zero, then verifies those settings before
and after capture.

Start the receiver first, with a new output file:

```sh
python3 examples/live-video/receive.py --backend libiio --uri ip:192.168.1.10 \
  --iio-python "$IIO_PYTHON" --receiver "$RX" --freq "$FREQ_HZ" --gain 30 \
  --rate 10000000 --symbol-rate 5000000 --modcod 13 --seconds 75 \
  --capture-report capture.json --ready ready.json --output received.ts 2>receiver.log
```

After `ready.json` reports `ready: true`, start a timed S3 run:

```text
spipins devkit
spiinput on ts
dvb FREQ_MHZ 13 5m 60 0 128
```

Replace `FREQ_MHZ` with its integer value and use the preset for your board.
Start the SPI source after the opening station ID; stop it several seconds
before the payload window ends so it can drain. The 60 s include both IDs;
`dvb` takes 1 to 540 s, and a longer run needs a longer `--seconds` on the receiver.
Keep the RF path contained until the board finishes.

Append `--player 'ffplay -infbuf -autoexit -i pipe:0'` to receive while saving. The player must
support MPEG-TS and the source codec (H.264 for cameras, MPEG-2 for the fixture).
`-infbuf` lets ffplay keep reading TS while playback catches up, preventing its
packet queue from blocking capture. The receiver still saves every TS byte and
enforces the IQ backlog checks.
Test playback of a recording first. Closing the player ends reception.

Then run `python3 examples/live-video/check_ts.py received.ts`. Check visible
moving video too; TS validity alone does not prove playback. Compare sender
and S3 `messages`, `ts_packets`, `payload_bytes`, `crc_chain` and `next_seq`;
require zero protocol errors, drops, timeouts, uncertain writes and late banks.

The E200 passed 10 and 15 MS/s; 20 MS/s fell behind near 15.5 MS/s. RX gain
30 dB worked in the box; 0 dB left MER near 4 dB. QPSK 3/4 at 10 Msym/s,
captured at 15 MS/s, lost lock four times in a minute: not reliable on the E200.
LeanDVB uses `--sr-tol 0.0005` to retain lock through dummy-heavy streams.
libiio exposes no sample timestamps/overflow count here; the capture report
checks host timing and queue health, while a source comparison checks TS loss.

`check_e200.py probe OUT --uri URI --mhz MHZ --iio-python PYTHON` measures
receive throughput with the S3 off. `check_e200.py keyed OUT` adds a contained
USB fixture rehearsal; pass the same URI/MHz/Python plus `--receiver`, `--port`,
`--probe-report PROBE/report.json --key --closed-box`. It defaults to the 8PSK
mode above and requires 90% exact contiguous recovery. It does not test SPI.

## UHD or saved IQ

UHD hardware needs GNU Radio's `uhd_rx_cfile` on PATH. Use explicit device args:

```sh
python3 examples/live-video/receive.py --args type=b200 --freq "$FREQ_HZ" \
  --gain 0 --antenna RX2 --rate 10000000 --symbol-rate 5000000 --modcod 13 \
  --seconds 75 --output received.ts
python3 examples/live-video/receive.py --iq dump.cf32 --rate 20e6 --output recovered.ts
```

For saved S3 IQ, pass `--format i16` and the dump's actual sample rate, symbol
rate and MODCOD. Synthetic GCS dumps contain no TS. `--help` and `--plan` open
no devices. Host checks: `python3 tests/test_live_video.py` and
`python3 tests/test_live_iio.py` (add `--receiver "$RX"` for model decode tests).

For optional external soft LDPC, supply a compatible, separately installed receiver/helper
pair and the known `--modcod`. Only normal frames and one helper are used; the stock
builder does not provide this pair. For example, this saved-IQ plan opens no devices:

```sh
python3 examples/live-video/receive.py --iq dump.cf32 --rate 50e6 --symbol-rate 20e6 \
  --modcod 13 --receiver "$SOFT_RX" --ldpc-helper "$LDPC_HELPER" --plan
```

Remove `--plan` to receive. The final report includes BCH results and balanced helper
EOF status; unavailable LDPC correction/parity counts stay `null`. TS continuity still
needs the packet checks above. Default reception keeps `--ldpc-bf 100`.
