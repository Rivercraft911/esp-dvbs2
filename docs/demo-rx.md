# Demo receiver

Clean file playback works through LeanDVB, MPEG-TS and FFmpeg. The weak-signal
10 Msym/s path is not ready for a reliable live demo with this LeanDVB build.
See [measurements](../results/2026-10-17-demo-rx/README.md) for speed and failures.
No E200, other lab device or serial port was accessed in developing this receiver.

## Run from a file

On macOS, from this checkout:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install numpy scipy imageio-ffmpeg
export LEANDVB=/Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb
.venv/bin/python host/demo_rx.py --demo
```

The last line is the single command for the demo. It generates a moving 960 × 480,
15 frame/s H.264 test card in `notes/demo-fixture/` on first use, encodes it through
`host/dvbs2.py`, and loops the resulting IQ at 5 Msym/s. Open the printed
`http://127.0.0.1:8765` if the browser does not open automatically. Everything is
local. FFmpeg is found through `FFMPEG`, PATH, or the `imageio-ffmpeg` package.
LeanDVB is a separate executable; no LeanSDR code is incorporated here.

The Flat button shows ordinary video. Select 360° for a 2:1 equirectangular input,
then drag or use arrow keys to look around and scroll or use +/− to zoom. Reset
restores the view. The test card exercises the projection and contains moving
video; it is not a photographed panorama. The browser uses WebGL and no CDN.
The dashboard displays JPEG frames from the decoded TS, up to 1280 pixels wide;
it has no audio player. Full resolution H.264 and audio stay in `--ts-out`.

```sh
.venv/bin/python host/demo_rx.py \
  --file /Users/river/Documents/stanford/SSI/esp-dvbs2/notes/captures/2026-10-16-8psk-board/13-10m.iq \
  --format s16 --scale 512 --sample-rate 40000000 --symbol-rate 10000000 --loop
```

Board dumps contain generic-stream test bytes, including optional callsign
headers. They exercise lock, FEC and spectrum but **cannot show video**. Use
`host/onair.py --stream 40 --callsign CALL` for their payload checks. Real TS
must be framed as TS at the transmitter, with the BBHEADER and packet CRC
adaptation in EN 302 307-1 clauses 5.1.4 to 5.1.6. Sending TS bytes inside the
existing generic-stream BBFRAME does not turn it into TS. Coordinate this with
the SPI engineer. `dvbs2.ts_bbframes()` is the tested reference.

To make a QPSK 3/4 or 10 Msym/s fixture:

```sh
.venv/bin/python host/rx_fixture.py notes/video-qpsk --modcod 7 --symbol-rate 10000000
.venv/bin/python host/demo_rx.py --file notes/video-qpsk/video.cf32 \
  --format f32 --sample-rate 20000000 --symbol-rate 10000000 --loop
```

Fixtures have cyclic pulse shaping. MPEG timestamps and TS continuity counters
restart at the file join; FFmpeg may report a discontinuity and lose some video
frames there. This is a replay artifact, not an RF measurement. Short keyframe
spacing gives recovery within the next group of pictures.

## Coordinator: qualify E200 first

These commands are provided for the coordinator. The author did not run them.
Keep the transmitter off for the first rate test. The E200 must already have
its agreed firmware and AD9364-mode configuration. This code does not change
firmware or reboot devices. Use libiio 0.25 tools, as in `host/sdr.py`.

The vendor lists **10 MS/s through ADI IIO** and **25 MS/s through UHD** on its
[product page](https://www.microphase.cn/productinfo/2299484.html). Its
[manual](https://antsdr-docs.microphase.cn/en/latest/device_and_usage_manual/ANTSDR_E_Series_Module/ANTSDR_E200_Reference_Manual/AntsdrE200_Reference_Manual.html)
lists Gigabit Ethernet. At one channel of int16 I + int16 Q, the unframed link
ceiling is `1e9 bit/s / (2 × 16 bit/sample) = 31.25 MS/s`. Ethernet, TCP and IIO
overhead lower it. **30 MS/s is an upper bound estimate, not a measured rate**.
61.44 MS/s requires 245.76 MB/s, or 1.966 Gb/s, so a finite DDR-buffered burst
cannot establish sustainable streaming. The claimed gap-free bursts remain
unverified here. A one-channel 20 MS/s stream needs 80 MB/s, or 640 Mb/s, and
fits the wire budget but exceeds the vendor's IIO rating.

Configure and read back the device, then measure 60 seconds of raw RX draining
without decoding (`--libiio PREFIX` is optional when tools are on PATH):

```sh
python3 host/rx_rate.py --uri ip:192.168.1.10 --sample-rate 20000000 --seconds 60 \
  --out notes/e200-20m-rate.json
```

Success: readbacks equal requested LO/rate/bandwidth, correct
`le:S12/16>>0` scan format, TX LO powered down, no reader errors, and at least
20 MS/s with no increasing input deficit. A drain test only measures delivery
rate. It cannot prove that a driver/FPGA did not silently drop ADC samples.
The tool's `delivery_pass` allows a 2% timing tolerance on the full run and
one-second windows after startup. Inspect the actual rates as well as that flag.
For continuity, the coordinator must also make a known counter/tag capture
and use the existing `host/capture.py` analysis, or inspect receiver overflow
counters. A DVB signal with uninterrupted TS continuity counters provides a
second check; FEC frame success alone cannot count frames lost before decoding.
Do not call a rate-only result gap-free.

Repeat at 10 MS/s if 20 MS/s fails. Use 30 MS/s only as an extra qualification
point; do not infer it from the Ethernet link speed. If libiio only sustains
10 MS/s, the available LeanDVB path is at most **5 Msym/s** at two samples per
symbol. A clean 10 Msym/s file benchmark does not remove this input limit.

```sh
python3 host/rx_rate.py --uri ip:192.168.1.10 --sample-rate 10000000 \
  --bandwidth 6750000 --seconds 60 --out notes/e200-10m-rate.json
python3 host/rx_rate.py --uri ip:192.168.1.10 --sample-rate 30000000 \
  --seconds 60 --out notes/e200-30m-rate.json
```

## Coordinator: live dashboard

After the coordinator has arranged the licensed frequency, at least 30 dB of
attenuation into the receiver, and authorized the transmitter separately:

```sh
.venv/bin/python host/demo_rx.py --e200 --uri ip:192.168.1.10 \
  --sample-rate 20000000 --symbol-rate 10000000 \
  --center 2420000000 --bandwidth 13500000 --gain 0 \
  --ts-out notes/live.ts --log notes/live-rx.log
```

This configures RX with verified readbacks, powers the TX LO down, checks the
scan format and launches exactly one `iio_readdev` RX process. It tees the same
IQ bytes internally to a bounded FFT snapshot queue and LeanDVB. The waterfall
can discard display updates; the IQ decode path never silently drops blocks.
Backpressure propagates to the reader. Watch input rate and growing lag.
`--tune HZ` specifies a measured carrier offset from the E200 center. Adjust
manual `--gain` while avoiding ADC clipping; 0 dB is a starting setting, not a
measured optimum. There is no AGC or automatic carrier search in this wrapper.
Use unique log/TS filenames: existing files are never overwritten.

For 5 Msym/s use `--sample-rate 10000000 --symbol-rate 5000000 --bandwidth 6750000`.
Success means a moving decoded picture, FRAME LOCK, increasing good frames,
few/no failed frames, received TS near its configured mux rate, and input
near the requested sample rate without steadily growing lag. Leave it running
for at least 60 seconds and include browser rendering in that check. A stopped
input turns metrics stale. Received bitrate counts all TS bytes, including null
packets; it is not the H.264 elementary-stream bitrate. MODCOD is decoded from
the stream. Symbol rate is configured, not measured. MER is LeanDVB's estimate,
not the known-payload MER calculation in `host/onair.py`. FFT power is
window-normalized dBFS per bin relative to `--scale`; it is not calibrated dBm.

A non-IIO producer may supply the same single-channel interleaved IQ on stdin:

```sh
YOUR_RX_PRODUCER | .venv/bin/python host/demo_rx.py --stdin --format s16 \
  --sample-rate 20000000 --symbol-rate 10000000 --center 2420000000
```

The producer must configure the actual sample rate, LO and channel layout.
Use this for a separately qualified UHD E200 path. UHD and libiio firmware
are different; do not assume changing the host command changes device firmware.

## S3 telemetry hook

`--telemetry notes/tx.jsonl` tails an existing regular file. The receiver never
opens a serial port. The coordinator may append one object per line:

```json
{"fill_slack_us": 24.5, "encoder_load_pct": 42.0}
```

Either field is optional. Units are explicit, invalid records are skipped,
and data older than 3 seconds is marked stale. Firmware integration is pending.

## Alternatives

The [SDRangel DATV documentation](https://github.com/f4exb/sdrangel/blob/master/plugins/channelrx/demoddatv/readme.md)
describes built-in soft LDPC decoding and TS over UDP, but still calls DVB-S2
experimental. Its [symbol-rate widget](https://github.com/f4exb/sdrangel/blob/master/plugins/channelrx/demoddatv/datvdemodgui.ui)
accepts 1 ksym/s to 1024 Msym/s. That UI range is not a throughput guarantee;
5 and 10 Msym/s still require a measured test on this Mac. RF tuning limits
come from its selected SDR source, here the E200, not the DATV demodulator.
Its convenient presets end at 2 Msym/s; the numeric field accepts higher rates.
It does not fix an underspeed libiio source. It is a candidate to test, not a
verified Monday solution.

A **Serit MiniTiouner with BATC Ryde** is the more direct hardware fallback.
[BATC's specifications](https://wiki.batc.org.uk/MiniTioune) give 143–2450 MHz,
66 ksym/s–30 Msym/s and DVB-S2 QPSK through 32APSK. Thus 2420 MHz and either
5 or 10 Msym/s are within its stated range. Check the particular tuner: older
Sharp units have a different range. [Ryde](https://wiki.batc.org.uk/Ryde_Receiver)
uses a Pi 4 and a USB tuner with HDMI output. It requires that hardware and
local gain/attenuation qualification. Its HDMI output is not directly integrated
into this laptop dashboard; a capture device or a separately verified TS feed
would be needed for the same panorama panel. An E200 waterfall would still need
its own single RX stream. No hardware receiver was tested in this work.

## Verify

```sh
.venv/bin/python tests/test_demo_rx.py
RX_INTEGRATION=1 .venv/bin/python tests/test_demo_rx.py
```

The optional integration test synthesizes H.264/TS IQ, checks decoded TS bytes
against the source, runs paced playback across loops, and fetches live JSON and
JPEG endpoints. It never uses hardware. Browser visual and interaction testing
was blocked by the local-origin approval gate, so it remains a coordinator check.
