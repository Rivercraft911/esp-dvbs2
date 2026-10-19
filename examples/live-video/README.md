# Live video over DVB-S2

Send a **9 Mb/s MPEG-TS** stream over SPI, then receive it with LeanDVB.
Use **8PSK 2/3 (MODCOD 13), 5 Msym/s, normal frames, pilots and .35 roll-off**:
9.68 Mb/s TS capacity in 6.75 MHz. The [Pi sender](../pi-sender/README.md) accepts
files or stdin; an existing PV-SPI v1 source can connect directly.

Measured in a closed box on 2026-10-18: S3 → E200 with Pluto firmware → LeanDVB
recovered 47,672 packets as an exact contiguous source slice, 99.6% of the file,
with zero continuity or PSI CRC errors. That test used USB input at about 2.8 Mb/s
and a mostly null-padded MPEG-2 fixture. A 2.4 Mb/s H.264 USB fixture also passed
live reception and playback. **Native 9 Mb/s camera input over SPI still needs
an end-to-end test.** USB intake tops out near
3.2 Mb/s; it preserves a finite file but cannot carry the live 9 Mb/s mux.

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
before the payload window ends so it can drain. The 60 seconds includes both
IDs. Keep the RF path contained until the board finishes.

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
captured at 15 MS/s, lost lock four times in a minute and is not yet reliable.
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
