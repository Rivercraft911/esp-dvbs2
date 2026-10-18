# Live video over DVB-S2

Send a **9 Mb/s MPEG-TS** file or ffmpeg stream over SPI with the [Pi sender](../pi-sender/README.md).
The example uses normal frames, QPSK 3/4 (**MODCOD 7**), pilots, **10 Msym/s** and roll-off
**0.35**: **14.520762 Mb/s** payload capacity in **13.5 MHz**. At 5 Msym/s it would carry
only 7.260381 Mb/s. A 9 Mb/s source leaves valid DVB-S2 dummy frames between data frames.

The verified route is **host-model TS → DVB-S2 IQ → LeanDVB → TS**, with MPEG-2 video.
It opens no devices. S3 sample dumps and on-chip SPI loopback have separate tests;
external SPI/cameras, live H.264 playback and RF reception remain unmeasured.

## RF-free dry run

Use the [host Python shell](../../README.md#quick-start) with numpy/scipy, Git, Make and a
C++ compiler. No GNU Radio, UHD, ffmpeg or player is required.
The [pinned LeanDVB builder](../self-check/README.md#prepare) installs the separate receiver:

```sh
python3 examples/self-check/build_receiver.py
export RX="$PWD/notes/leansdr/src/apps/leandvb"
python3 examples/live-video/dry_run.py notes/video-dry --receiver "$RX"
python3 examples/live-video/check_ts.py notes/video-dry/received.ts \
  --reference notes/video-dry/source.ts
```

Use a new output directory each time. This generates one second of animated MPEG-2 video
in a 9 Mb/s TS, encodes it with the host model into 20 MS/s IQ, and invokes `receive.py --iq`.
Success requires at least 90% recovery as an exact contiguous source slice, zero continuity
and PSI CRC errors, PAT/PMT, a declared video PID with PES starts, and PCR.
Acquisition and buffering can trim the ends; no packets may disappear in the middle.
IQ, both TS files, receiver logs and `report.json` stay in the output directory.
`--ts video.ts` substitutes another complete 9 Mb/s TS source.

## Prepare a sender

On a Linux Pi/CM5, enable SPI0 and install the [sender dependencies](../pi-sender/README.md)
(`spidev` and `gpiod>=2,<3`). Identify the GPIO chip with `gpioinfo`.
Connect **Pi BCM GPIO25 (header pin 22) to S3 GPIO8 (READY)**, plus SCK, CS, MOSI and GND
as in the [complete wiring table](../../docs/spi-input.md#pins).
Use mode 0 at 20 MHz: one raw 1,332-byte message per CS, without a command prefix.
The ideal payload ceiling is 19.395726 Mb/s before software overhead.

For a self-contained file source, generate 35 seconds of test video on the Pi:

```sh
python3 examples/live-video/make_ts.py demo.ts --frames 875
python3 examples/pi-sender/send.py --vector  # 1332 bytes, CRC 0x51be328d; no hardware
```

For H.264 instead, install ffmpeg with `libx264`, and encode your source into a new file:

```sh
ffmpeg -i input.mp4 -t 35 -an -c:v libx264 -preset ultrafast -tune zerolatency \
  -b:v 4M -maxrate 4M -bufsize 8M -pix_fmt yuv420p -muxrate 9000000 -f mpegts demo.ts
```

Optional PigeonVision camera software can supply the same PV-SPI v1 messages; its camera
setup is outside this example. The file and ffmpeg routes use only the shipped sender.

## Live reception

Install **GNU Radio with gr-uhd and UHD**, which supplies the
[`uhd_rx_cfile` application](https://github.com/gnuradio/gnuradio/blob/aee9fd3f79389c4282a98e8d62c8405c73fd91df/gr-uhd/apps/uhd_rx_cfile),
and put it on PATH. UHD alone does not supply this flowgraph.

```sh
command -v uhd_rx_cfile
export RX=/absolute/path/to/leandvb
```

First follow [contained RF and station identification](../../docs/transmitting.md).
Set the assigned callsign, confirm receiver input-power limits and port names, and use one
serial client. Choose an authorized integer `FREQ_MHZ` in 2390–2450 and the matching
`FREQ_HZ`. These commands access hardware and are separate from the dry run.

1. **Receiver host**, start before the S3 (B200 shown):

   ```sh
   python3 examples/live-video/receive.py --args type=b200 --freq "$FREQ_HZ" \
     --gain 0 --antenna RX2 --seconds 75 --output received.ts 2>receiver.log
   ```

2. **S3 console**, after setting the station callsign, replace `FREQ_MHZ` with its value:

   ```text
   spipins devkit
   spiinput on ts
   dvb FREQ_MHZ 7 10m 60 0 128
   ```

3. **Pi/CM5**, once the S3 is running, send the prepared file:

   ```sh
   python3 examples/pi-sender/send.py demo.ts --mbps 9 --gpiochip /dev/gpiochip4
   ```

   To stream from ffmpeg instead:

   ```sh
   ffmpeg -re -i demo.ts -c copy -muxrate 9000000 -f mpegts pipe:1 |
     python3 examples/pi-sender/send.py - --mbps 9 --gpiochip /dev/gpiochip4
   ```

Stop the sender several seconds before the S3 payload window ends. The 60 s limit includes
start/end Morse ID; the receiver must reacquire after the first ID. Keep the contained RF
path connected until the timed run finishes.

Reception requests **20 MS/s**, two samples/symbol, within the E200's 20 MHz bandwidth.
For an E200 with UHD firmware use `--args "addr=$E200_ADDR"`. Start at manual 0 dB RX gain
and adjust from measured level without clipping. Samples are `sc16` on the wire, signed
int16 into LeanDVB (`--s16`), about 80 MB/s. Verify the actual sample-rate readback;
reject UHD overflows (`O`), sequence drops and rate coercion. Use the SDR's rate, not the
S3's 40 MS/s DAC rate.

Add `--player 'ffplay -i pipe:0'`, `--player 'cvlc -'`, or
`--player 'gst-launch-1.0 -q playbin uri=fd://0'` to view while saving. Install the player,
TS demuxer and the source codec's decoder first (H.264 for the ffmpeg route, MPEG-2 for the
test file). Output files must be new. Diagnostics go to stderr; stdout carries TS only.

Run `python3 examples/live-video/check_ts.py received.ts`. Compare drained sender/S3
`messages`, `ts_packets`, `payload_bytes`, `crc_chain` and `next_seq`; require zero protocol
errors, drops, timeouts, uncertain writes, late S3 banks, underruns and encoder failures.
Verify moving video as well: valid TS alone does not prove playback or RF operation.

## Saved IQ

```sh
python3 examples/live-video/receive.py --iq dump.cf32 --rate 20e6 --output recovered.ts
```

For an S3 `dvbdump`, use `--format i16` and its reported sample rate, symbol rate and
MODCOD. For example, a non-decimated 10 Msym/s dump is 40 MS/s. Synthetic GCS dumps contain
no TS. [Common IQ format aliases](../../docs/transmitting.md#console).
`--help` and `--plan` do not open devices.

References: [UHD streaming](https://files.ettus.com/manual/page_stream.html),
[pinned LeanDVB](https://github.com/pabr/leansdr/tree/84c59e1c7a1a79338d5722d63f28640cc9d350f3),
[MPEG-2 video](https://www.itu.int/rec/T-REC-H.262).
