# Demo receiver

The demo defaults to **QPSK 3/4 at 10 Msym/s**, with a 20 MS/s receive stream.
The [file measurements](../results/2026-10-17-demo-rx/README.md) give 2.47× real
time at Es/N₀ 15 dB, with every attempted frame recovered. 8PSK 2/3 at the same
rates gives 0.58× and cannot keep up. These are decoder file timings, not RF or
complete noisy video pipeline qualification. No SDR, serial port or lab network
device was accessed in this follow-up. The coordinator must qualify the full
receive chain for 60 seconds before the demo.

## macOS setup

B200 uses SoapySDR → SoapyUHD → UHD. E200 uses SoapySDR → SoapyPlutoSDR →
libiio, or the existing libiio command-line backend. Install the B200 stack:

```sh
brew install soapysdr uhd ffmpeg
brew tap pothosware/pothos
brew install pothosware/pothos/soapyuhd
# Match the Python ABI used by the installed SoapySDR formula.
SOAPY_PYTHON="$(brew deps soapysdr | sed -n '/^python@/p' | tail -n 1)"
"$(brew --prefix "$SOAPY_PYTHON")/bin/python${SOAPY_PYTHON#python@}" -m venv --system-site-packages .venv
.venv/bin/python -m pip install numpy scipy
.venv/bin/python -c 'import SoapySDR; print(SoapySDR.__file__)'
export LEANDVB=/Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb
```

If `.venv` already uses another Python version, create the matching environment
at `notes/rx-venv` instead and substitute that path. Do not install an unrelated
`SoapySDR` wheel from PyPI. Homebrew's Python bindings and driver modules must
match the installed SoapySDR ABI. Software-only `SoapySDRUtil --info` lists
modules without probing a radio. UHD may need its matching FPGA/firmware images:
run `"$(brew --prefix uhd)/lib/uhd/utils/uhd_images_downloader.py"` during setup.
The receiver does not update E200 firmware or its AD9364-mode configuration.
[Homebrew SoapySDR](https://formulae.brew.sh/formula/soapysdr),
[UHD](https://formulae.brew.sh/formula/uhd),
[SoapyUHD formula](https://github.com/pothosware/homebrew-pothos/blob/master/soapyuhd.rb).

For E200, build the upstream Pluto module against libiio **0.25**. This avoids
assuming an unavailable Homebrew Pluto formula or mixing libiio 0.x/1.x APIs.
Homebrew supplies the build dependencies; all source/build files stay in notes/:

```sh
brew install cmake pkgconf libxml2 libusb
RX_IIO="$PWD/notes/libiio-0.25"
git clone --branch v0.25 --depth 1 https://github.com/analogdevicesinc/libiio notes/libiio-src
cmake -S notes/libiio-src -B notes/libiio-build \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_INSTALL_PREFIX="$RX_IIO" \
  -DCMAKE_PREFIX_PATH="$(brew --prefix);$(brew --prefix libxml2)" \
  -DWITH_LOCAL_BACKEND=OFF -DWITH_TESTS=ON
cmake --build notes/libiio-build -j 4
cmake --install notes/libiio-build
git clone --depth 1 https://github.com/pothosware/SoapyPlutoSDR notes/SoapyPlutoSDR
cmake -S notes/SoapyPlutoSDR -B notes/pluto-build \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_INSTALL_PREFIX="$PWD/notes/soapy-pluto" \
  -DCMAKE_PREFIX_PATH="$RX_IIO;$(brew --prefix soapysdr);$(brew --prefix)"
cmake --build notes/pluto-build -j 4
cmake --install notes/pluto-build
export SOAPY_SDR_PLUGIN_PATH="$PWD/notes/soapy-pluto/lib/SoapySDR/modules$(SoapySDRUtil --abi-version)"
```

`libad9361` is optional in upstream SoapyPlutoSDR; these commands do not install
it. E200 firmware must already expose the requested rate. The legacy backend
uses `--libiio "$RX_IIO"` and does not require SoapySDR or its Python bindings.
These install instructions were checked against upstream build descriptions;
the installs and device initialization were not run here.
[SoapyPlutoSDR dependencies/build](https://github.com/pothosware/SoapyPlutoSDR),
[libiio v0.25](https://github.com/analogdevicesinc/libiio/tree/v0.25).

## Run from a file

On macOS, from this checkout:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install numpy scipy imageio-ffmpeg
export LEANDVB=/Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb
.venv/bin/python host/demo_rx.py --demo
```

The last line is the single command for the demo. It generates a moving 960 × 480,
15 frame/s H.264 test card in `notes/demo-qpsk-10m/` on first use, encodes it through
`host/dvbs2.py`, and loops QPSK 3/4 IQ at 10 Msym/s. An older
`notes/demo-fixture/` is ignored. Open the printed
`http://127.0.0.1:8765` if the browser does not open automatically. Everything is
local. FFmpeg is found through `FFMPEG`, PATH, or the `imageio-ffmpeg` package.
LeanDVB is a separate executable; no LeanSDR code is incorporated here.
Use the local patched build named above. Its `-d` output supplies the custom
`S2FEC ... bch_errors=...` records used for frame counts, and `--fd-info 2`
supplies `FRAMELOCK` and `MER`. An upstream build without those diagnostics
cannot provide the same dashboard counters. The measured binary's SHA-256 is
recorded with each benchmark; its upstream base is LeanSDR work revision
`84c59e1c7a1a79338d5722d63f28640cc9d350f3`.

The Flat button shows ordinary video. Select 360° for a 2:1 equirectangular input,
then drag or use arrow keys to look around and scroll or use +/− to zoom. Reset
restores the view. The test card exercises the projection and contains moving
video; it is not a photographed panorama. The browser uses WebGL and no CDN.
Without WebGL it shows flat decoded video and disables the panorama controls.
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

To make another QPSK 3/4 fixture explicitly:

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

## B200 bandwidth and decoder speed

The [B200 product specification](https://www.ettus.com/all-products/ub200-kit/)
states 56 MHz instantaneous RF bandwidth and 61.44 MS/s continuous quadrature
streaming over USB 3.0. This is a published capability, not a measurement of the
incoming unit or this Mac. The B200 uses AD9364; B210 uses AD9361. Both fit this
single-RX-channel wrapper. USB 2.0 is unsuitable for these proposed rates.
[UHD B2x0 manual, comparative features and clock-rate sections](https://files.ettus.com/manual/page_usrp_b200.html).

For roll-off α = 0.35 and two complex samples/symbol, occupied bandwidth is
`B = (1+α) Rs` in Hz, and input sample rate is `Fs = 2 Rs` in sample/s.
Thus the published receiver ceilings give
`Rs ≤ min(56 MHz/1.35, 61.44 MS/s/2) = 30.72 Msym/s`.
61.44 MS/s of int16 I + int16 Q uses `Fs × 4 = 245.76 MB/s` over USB;
CF32 expands that to 491.52 MB/s in host memory. Neither is a DDR burst claim.

| QPSK 3/4 Rs | Fs | Occupied B | TS capacity | Clean RTF estimate | 15 dB RTF estimate |
|---|---|---|---|---|---|
| 10 Msym/s | 20 MS/s | 13.5 MHz | 14.521 Mb/s | 2.69× measured | 2.47× measured |
| 20 Msym/s | 40 MS/s | 27 MHz | 29.042 Mb/s | 1.35× | 1.24× |
| 26.667 Msym/s | 53.333 MS/s | 36 MHz | 38.722 Mb/s | 1.01× | 0.93× |

TS capacity is `(48328 bit / 33282 symbol) × Rs`, with normal frames and pilots,
from the [reference/standard derivation](../results/2026-10-17-demo-rx/README.md#complete-backend).
Higher-rate RTF is the **estimate** `RTF10 × 10 Msym/s / Rs`, assuming constant
work per symbol. It omits live driver and browser costs. The clean/15 dB
compute ceilings are 26.9/24.7 Msym/s by this scaling. Therefore **20 Msym/s is a
plausible qualification target**; **26.67 Msym/s has essentially no clean margin
and falls behind at 15 dB**. B200 bandwidth permits both; the measured LeanDVB
speed does not establish a reliable 26.67 Msym/s live demo. At worse noise the
10 Msym/s mode also loses margin. Stay with 10 Msym/s for Monday.

To qualify 20 Msym/s later add `--symbol-rate 20000000` (40 MS/s, 27 MHz filter).
For 80/3 Msym/s use `--symbol-rate 26666667 --sample-rate 53333334 --bandwidth 36000001`.
These set receiver rates only, not transmitter MODCOD or rate.

## Coordinator: live dashboard

After the coordinator has arranged the licensed frequency, at least 30 dB of
attenuation into the receiver, and authorized the transmitter separately:

```sh
.venv/bin/python host/demo_rx.py --device b200 --center 2420000000 --gain 0 \
  --ts-out notes/monday-b200.ts --log notes/monday-b200.log
# Same command and defaults, with E200/SoapyPlutoSDR selected:
.venv/bin/python host/demo_rx.py --device e200 --uri ip:192.168.1.10 \
  --center 2420000000 --gain 0 --ts-out notes/monday-e200.ts --log notes/monday-e200.log
# Existing libiio 0.25 path, also available as legacy --e200:
.venv/bin/python host/demo_rx.py --device e200 --backend libiio --libiio "$RX_IIO" \
  --uri ip:192.168.1.10 --center 2420000000 --gain 0 \
  --ts-out notes/monday-iio.ts --log notes/monday-iio.log
```

Select **QPSK 3/4, 10 Msym/s, pilots, roll-off 0.35, MPEG-TS framing** at the
transmitter separately. The receiver detects MODCOD; it cannot change the
transmitter. The default RX settings are 20 MS/s, 13.5 MHz RF filter and manual
0 dB gain. B200 selects RX2, channel 0, `driver=uhd,type=b200`; add `--serial SERIAL`
for a particular unit. E200 selects A_BALANCED, channel 0,
`driver=plutosdr,uri=ip:192.168.1.10`. Use the appropriate physical RX connector.
Both use normalized CF32 inside the host; SoapyUHD requests `WIRE=sc16` over USB.
Soapy readbacks tolerate 1 ppm sample-rate rounding, 1 Hz LO rounding, 1% filter
rounding and 0.5 dB gain steps, and otherwise fail before streaming. Requested
and actual settings appear in `/api/state` and the diagnostic log. The wrapper
opens only one RX stream. Overflow, corruption, unexpected stream end or five
seconds without samples fails visibly rather than silently hiding IQ loss.
SoapyPlutoSDR cannot prove absence of drops its driver does not report.
[SoapyUHD stream arguments](https://github.com/pothosware/SoapyUHD),
[SoapyPlutoSDR settings](https://github.com/pothosware/SoapyPlutoSDR/blob/master/PlutoSDR_Settings.cpp).

The libiio backend verifies exact readbacks and scan format, powers the TX LO
down and launches one continuous `iio_readdev`. The Soapy paths open no TX
stream; they do not provide that libiio TX-LO powerdown check. All paths tee
the same IQ internally to a bounded spectrum snapshot queue and LeanDVB. Only
display updates may be discarded. Decoder backpressure reaches the device;
Soapy overflows stop reception. Watch input rate and growing lag.
`--tune HZ` specifies a measured carrier offset from RX center. Adjust manual
`--gain` to avoid clipping; 0 dB is a starting setting, not a measured optimum.
There is no AGC or automatic carrier search in this wrapper.
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
JPEG endpoints. It never uses hardware. The follow-up static render check runs
the actual dashboard script in jsdom and
renders the 2D spectrum/waterfall on CPU canvases. It checks video API calls,
flat fallback, panorama drag/keys/zoom/reset, fullscreen requests, stale values,
source errors and disconnects, without HTTP or device access:

```sh
.venv/bin/python tests/test_rx_device.py
npm install --prefix /tmp/rx-dom jsdom @napi-rs/canvas
NODE_PATH=/tmp/rx-dom/node_modules node tests/test_demo_rx_render.cjs
```

Headless Chrome was attempted and aborted under the process sandbox. This
static route caught and fixed blank video when WebGL is unavailable, and errors
hidden in that mode. GPU shader output, responsive layout and actual fullscreen
still need a coordinator browser check. The static test is in CI.
