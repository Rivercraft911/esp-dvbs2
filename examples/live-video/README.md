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

### E200 with Pluto firmware (libiio)

Use gigabit Ethernet and an explicit URI, **`ip:192.168.1.10`**. The capture Python
needs pylibiio; 15 MS/s interpolation also needs numpy/scipy (Radioconda has these).
No discovery, flashing or TX buffer is used. The unchanged `Radio` implementation from
`over-the-air/capture_iio.py` powers down TX LO, sets every RF TX gain to **-89.75 dB**
and every DDS raw to **0**, with readbacks before RX setup, before the buffer and after
capture. Cleanup reapplies the inhibits; it never restores previous TX settings.

| S3 mode (pilots, .35 roll-off) | TS capacity | Occupied | E200 capture |
|---|---:|---:|---:|
| 8PSK 2/3, MODCOD 13, 5 Msym/s | 9.68 Mb/s | 6.75 MHz | 10 MS/s |
| QPSK 3/4, MODCOD 7, 10 Msym/s | 14.52 Mb/s | 13.5 MHz | 15 MS/s (1.5 samples/symbol); 20 MS/s exceeded E200 Ethernet throughput |

Both fit the 9 Mb/s demo mux. Firmware accepts `dvb FREQ_MHZ 13 5m 60 0`
with **SPI or USB TS input**: named `5m` selects 8 samples/symbol at the 40 MS/s DAC,
and external input uses the TS encoder with PSRAM workspace. See [SPI modes](../../docs/spi-input.md#which-wire-for-which-mode).
USB's measured ~3.2 Mb/s intake inserts dummy frames; it cannot sustain a 9 Mb/s live
source. A finite 9 Mb/s TS file is sent more slowly, with its bytes preserved.

The **coordinator**, with the S3 unkeyed, runs the receive-only probe (~30 s/rate).
Set `FREQ_MHZ` to the assigned integer MHz, `PORT` to the native S3 USB port, `RX` to
LeanDVB and `IIO_PYTHON` to the pylibiio Python (e.g. `$HOME/radioconda/bin/python`).
Use new output directories; close other serial clients.

```sh
python3 examples/live-video/check_e200.py probe notes/e200-probe \
  --uri ip:192.168.1.10 --mhz "$FREQ_MHZ" --iio-python "$IIO_PYTHON"
```

Then, **only inside the closed box**, with E200 connected as receive-only and the
assigned callsign already stored, run (~one minute per command):

```sh
python3 examples/live-video/check_e200.py keyed notes/e200-8psk \
  --uri ip:192.168.1.10 --mhz "$FREQ_MHZ" --iio-python "$IIO_PYTHON" \
  --receiver "$RX" --port "$PORT" --probe-report notes/e200-probe/report.json --key --closed-box

python3 examples/live-video/check_e200.py keyed notes/e200-qpsk --modcod 7 \
  --uri ip:192.168.1.10 --mhz "$FREQ_MHZ" --iio-python "$IIO_PYTHON" \
  --receiver "$RX" --port "$PORT" --probe-report notes/e200-probe/report.json --key --closed-box
```

QPSK refuses before opening the S3 unless a matching 20 or 15 MS/s probe passes;
20 is preferred, otherwise 15 is interpolated continuously with FIR history/lookahead.
The probe discards IQ: its pass is a prerequisite, not proof the decoder can keep up.
The keyed command generates 8 seconds of 9 Mb/s MPEG-2 TS (or takes `--ts FILE`),
starts `receive.py --backend libiio`, waits for one second of delivered IQ, then invokes
`examples/usb-send/send.py` for a 60-second timed run including station IDs. It requires
finite-source EOF, matching USB counts/CRC chain, healthy encoder deadlines, zero TS
continuity/PSI errors and **at least 90% as one byte-exact contiguous sent TS slice**.
Only acquisition/EOF trimming is allowed. `source.ts`, live `received.ts`, capture/sender
reports and logs remain in the evidence directory. Add `--player 'ffplay -i pipe:0'`.

There are no sample timestamps/overflow counters in this libiio API: `overflows` stays
`null`. Capture reports refill wall time versus buffer duration, cumulative host lag,
queue depth and queue age. A refill over 1.5 buffer durations, lag beyond four kernel
buffers, full host queue, short read or transport failure fails the run. A bounded queue
separates refills from decoder writes; it never silently drops IQ. Payload comparison
detects losses that host timing and wrapping TS counters can miss. LeanDVB retains
`--sr-tol 0.0005`, `--fastdrift` and `--ldpc-bf 100` for dummy runs and clock mismatch.
`--plan` on either command opens no devices. Host checks: `python3 tests/test_live_iio.py`.
Add `--receiver "$RX"` to that test for model IQ → int16 → LeanDVB proofs of both
MODCODs, including dummy-heavy USB pacing and the 15 MS/s interpolation path.

### UHD

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
