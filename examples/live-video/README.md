# Live video over DVB-S2

A CM5 camera running `pv-capture` sends MPEG-TS over SPI to an ESP32-S3;
a UHD receiver and LeanDVB recover TS for a file or player. Normal frames,
QPSK 3/4 (MODCOD **7**), pilots on, **10 Msym/s**, roll-off **0.35**.

The TS capacity is `(48408-80) * 10e6 / 33282 = 14.520762 Mb/s`:
32,400 data symbols + 90 header + 22×36 pilots per frame. The camera's two
4 Mb/s H.264 streams fit in its **9 Mb/s total CBR TS**, including tables,
metadata and nulls. Raw 20 MHz SPI has an ideal **19.395726 Mb/s** payload
ceiling, before software overhead. Keep this mode; 5 Msym/s QPSK 3/4 would
carry only 7.260381 Mb/s. The 9 Mb/s producer leaves DVB-S2 dummy frames
between data frames; these are expected and do not enter recovered TS.

See [SPI wiring and the unchanged pv-capture contract](../../docs/spi-input.md#cm5-with-unchanged-pv-capture).
Use `spiinput pv`, mode 0, 20 MHz, one raw 1,332-byte message per CS,
READY on GPIO25. Do not add a command prefix or change the camera transport.

## Prepare

Install Python 3, numpy/scipy (dry run only), UHD's `uhd_rx_cfile`, and
[LeanDVB](../self-check/README.md#prepare). Set `RX` to the receiver executable
and put UHD tools on `PATH`. No device is opened by `--help`, `--plan`, or `--iq`.
For playback install a TS demuxer and H.264 decoder as well as a player.
GStreamer needs `mpegtsdemux`, an H.264 decoder, and a video sink; its base
plugins alone cannot play this feed. File output works without a player.

## Three live commands

First follow [contained RF and station identification](../../docs/transmitting.md).
Keep RF in coax through suitable attenuators or a closed shielded box until
the assigned station callsign is set up. Confirm input-power limits and port
names. One coordinator owns the board; no other serial client may run.
Set an operator-approved `FREQ_MHZ`/matching `FREQ_HZ`; the S3 accepts integer
2390–2450 MHz. These commands are for the operator, not part of the dry run.

1. **Receiver host**, start before the S3 (B200 below). For an E200 with UHD
   firmware replace `type=b200` with `addr=$E200_ADDR`:

   ```sh
   python3 examples/live-video/receive.py --args type=b200 --freq "$FREQ_HZ" \
     --gain 0 --antenna RX2 --seconds 75 --output received.ts 2>receiver.log
   ```

2. **S3 console**, select the correct board pins and fresh input, then transmit
   at the same center frequency (replace `FREQ_MHZ` with its integer value):

   ```text
   spipins devkit
   spiinput pv
   dvb FREQ_MHZ 7 10m 60 0 128
   ```

3. **CM5**, after the S3 is running, use its existing resolved camera config
   and a fresh session directory; stop capture with Ctrl-C after about 35 s:

   ```sh
   pv-capture --config capture.json
   ```

Stop capture several seconds before the S3's payload window ends. The 60 s
S3 limit includes start/end Morse IDs; the receiver reacquires after the first
ID. Never disconnect the RF path while a timed S3 run is still active.

The receiver requests **20 MS/s**, two samples/symbol, for **13.5 MHz** occupied
bandwidth, within the E200's 20 MHz limit. It uses manual **0 dB RX gain** as a
contained-path starting point, `sc16` on the wire and interleaved signed int16
to LeanDVB (`--s16`), about **80 MB/s**. Increase gain only from measured level;
do not clip. Check UHD's actual sample-rate readback is 20 MS/s and reject
overflows (`O`), sequence drops or rate coercion. Do not use the 40 MS/s S3 DAC
rate as the SDR rate. No sample decimation is used in this receive path.

Add `--player 'gst-launch-1.0 -q playbin uri=fd://0'` to view while saving,
or use `--player 'ffplay -i pipe:0'` / `--player 'cvlc -'` if installed.
Output files must be new. Diagnostics go to stderr; stdout is TS only.
The UHD IQ FIFO keeps its startup text out of the samples.

## RF-free dry run and success checks

```sh
python3 examples/live-video/dry_run.py notes/video-dry --receiver "$RX"
python3 examples/live-video/check_ts.py notes/video-dry/received.ts \
  --reference notes/video-dry/source.ts
```

This synthesizes one second of valid animated MPEG-2 video in a 9 Mb/s TS,
uses the repo's host encoder/model for QPSK 3/4 and .35 RRC IQ at 20 MS/s,
inserts idle dummy frames, and invokes **the same receive.py** with `--iq`.
No board, serial port, SDR or RF is involved. `--ts video.ts` uses another
complete 9 Mb/s TS source. `report.json`, `receiver.log`, IQ and both TS files
remain in the new directory. Success requires at least 90% recovery as an
exact contiguous source slice, zero continuity/PSI CRC errors, PAT/PMT, a
declared video PID with PES starts, and PCR. Leading acquisition and trailing
encoder/decoder buffering can trim packets. Nothing may disappear in the middle.

For saved IQ: `receive.py --iq dump.cf32 --rate 20e6 --output recovered.ts`.
An S3 `dvbdump` is **signed int16**, so use `--format s16` and its reported
sample rate (e.g. `--rate 40e6` for a non-decimated 10 Msym/s dump).
Match its actual symbol rate and MODCOD too. Synthetic GCS dumps contain no TS.

On live output run `python3 examples/live-video/check_ts.py received.ts`.
The camera should advertise H.264 video PIDs **256/257**, private metadata
PID **258**, and repeated PAT/PMT; the synthetic video has one MPEG-2 PID 256.
Compare drained sender/S3 messages, TS packets, bytes, CRC chain and next
sequence, with zero protocol errors/drops/timeouts/uncertain writes. Require
zero S3 late banks/underruns/encoder failures. Finally verify visible moving
video; TS checks alone do not prove H.264 decoding, timing or live RF operation.

Format/streaming references: [UHD](https://files.ettus.com/manual/page_stream.html),
[LeanDVB](https://github.com/pabr/leansdr), [MPEG-2 video](https://www.itu.int/rec/T-REC-H.262).
