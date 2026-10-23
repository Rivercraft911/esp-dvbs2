# Contained RF check

For a real file with erasure recovery, use the
[file check](../../docs/file-link.md#keyed-closed-faraday-box), `file_check.py`.
It wraps the USB FEC sender, this libiio RX worker, LeanDVB and two SHA-256-checked
unpack runs, including deliberate receive-frame drops, in one command.

Capture with UHD or a Pluto-firmware ADALM-Pluto / ANTSDR E200 (libiio), decode with
LeanDVB, and compare every recovered payload byte against the synthetic source.
E200 runs in a closed box recovered exact payloads streaming at 2.5 MS/s
([QPSK 1/2, 1 Msym/s](../../docs/measurements/ota/ota1-qpsk12-1m/report.json)) and
10 MS/s ([QPSK 3/4, 5 Msym/s](../../docs/measurements/ota/ota2-qpsk34-5m/report.json)),
and at 20 MS/s from a single 16 M sample device buffer
([QPSK 3/4, 10 Msym/s](../../docs/measurements/ota/ota4-qpsk34-10m-burst/report.json)).
Through this script's libiio backend, 2,937 frames of QPSK 3/4 at 5 Msym/s came back exact
([report](../../docs/measurements/ota/pluto-backend-qpsk34-5m/report.json)). Host tests use
simulated devices only.

Read [Transmitting](../../docs/transmitting.md). Use a **closed Faraday box with
both devices inside**, or **coax with attenuators** (at least 30 dB initially).
Antennas stay inside the closed box. Check receiver input limits and record the
path, attenuation, connectors and gain. Gain indices are not calibrated power.
For anything radiated, the operator needs the required licence, an assigned
callsign stored in the S3, and an authorized frequency/emission containing the
whole signal. A box does not establish zero leakage or licence authority.
2420 MHz is a bench default, not band permission.

`--key` authorizes the S3's timed transmission. Keep containment connected until
it finishes: Ctrl-C stops host capture but **cannot stop already keyed firmware**.
No discovery, reset, firmware changes or callsign writes occur. The exact serial
port is locked; `info` must return a valid stored callsign. `--callsign` optionally
cross-checks it. The keyed seconds include both Morse IDs; short runs are refused.
`--seconds` takes 1 to 60 here; the firmware itself allows up to 540 s.
`spiinput off` selects synthetic input and disables USB ingress for this run.

## Pluto / E200 with Pluto firmware

Use host Python with `numpy scipy matplotlib pyserial` and a LeanDVB build supporting
this repository's FEC diagnostics (see [host decoder](../../host/decode.py)). Set
`RX` to that executable. The isolated worker defaults to
`~/radioconda/bin/python`; use `--iio-python /path/to/python` for another
Python with **pylibiio** and its native libiio library. Radioconda includes these
[ADI Python bindings](https://github.com/analogdevicesinc/libiio/tree/v0.26/bindings/python).
UHD bindings are not needed for this backend.

From the repository root, with the owner's configured URI and serial port:

```sh
export IIO_URI=ip:192.168.1.10  # E200 example; Pluto commonly uses ip:192.168.2.1
export RX=/path/to/leandvb

# Device-free plan; creates no evidence files.
python3 examples/over-the-air/check.py --backend libiio --uri "$IIO_URI" --plan

# First check: QPSK 3/4, 5 Msym/s, 10 MS/s streaming.
# Operator runs only after closing the box / connecting the attenuated path.
python3 examples/over-the-air/check.py notes/ota-pluto-first \
  --backend libiio --uri "${IIO_URI:?set IIO_URI}" --receiver "${RX:?set RX}" \
  --port /dev/cu.usbmodem1101 --mhz 2420 --modcod 7 --rate 5m \
  --sample-rate 10000000 --rx-gain 30 --gain 0 --peak 128 --seconds 30 --key

# Wider check: one contiguous device buffer after the leading Morse ID.
python3 examples/over-the-air/check.py notes/ota-pluto-burst \
  --backend libiio --uri "${IIO_URI:?set IIO_URI}" --receiver "${RX:?set RX}" \
  --port /dev/cu.usbmodem1101 --modcod 7 --rate 10m --sample-rate 20000000 \
  --burst 16000000 --rx-gain 30 --seconds 30 --key
```

Each output directory must be new. Explicit `usb:BUS.ADDR` URIs also work; bare
`ip:` / `usb:` and automatic discovery are refused. Use the physically connected
RX connector. Keep other SDR clients closed for the entire run.

The SDR is receive-only even when its TX connector has no load. Before RX setup,
the worker sets **TX LO powerdown = 1**, **every TX hardwaregain = -89.75 dB** and
**every DDS raw = 0**, then reads every value back. It reapplies these inhibits
after RX setup, verifies them immediately before creating an RX buffer, and
checks again after capture. Cleanup reapplies the inhibits on success or failure.
Missing attributes, failed writes, nonfinite values,
or any mismatch refuse capture. There is no TX buffer, TX tuning or restore of
previous TX settings. RX LO, sample rate, analog bandwidth and manual gain also
require matching readbacks. Unsupported sample layouts are refused.

| Symbol rate | Native capture rate | Transport evidence / choice |
|---|---|---|
| 1 Msym/s (`--rate 1m`, e.g. `--modcod 4`) | 2.5 MS/s | Streaming recovered exact payload |
| 5 Msym/s (`--rate 5m`, MODCOD 7) | 10 MS/s | Streaming recovered exact payload; start here |
| 10 Msym/s (`--rate 10m`, MODCOD 7) | 20 MS/s | Streaming lost samples; use a 16 M sample burst (0.8 s) |

Streaming above 10 MS/s is refused. Sustainable throughput depends on the host
and link; even 10 MS/s is not universal, especially over Pluto USB. libiio here
exposes no sample timestamps or hardware overflow counter: `overflows` is `null`,
not an invented zero. Transport errors and short buffers fail; exact-payload
comparison rejects missing frames inside the recovered slice. This does not
prove continuity in undecoded parts of a stream.

Burst mode queues **one RX kernel buffer**, after the stored callsign's Morse
duration plus `--burst-margin` (default 2.3 s for firmware startup). It refills
exactly once. The whole burst must fit between both IDs; a conservative check
against board completion timing rejects capture before the leading ID ended or
transfer completion too late to prove capture preceded the trailing ID.
Increase the margin / keyed duration if startup is slower. The 16 M sample limit
is the tested allocation; transfer can take longer than the RF sample window.

Default analog bandwidth is 1.15 times the nominal RRC bandwidth (roll-off 0.35).
`--bandwidth HZ` overrides it. `20m`, `26m667` and `33m333` are experimental libiio
burst choices, **not validated decodes** on this path; native rates are capped at
61.44 MS/s. Those rates have decoded from B200 captures taken another way
([keyed table](../../results/2026-10-23-tx-table/README.md)).
A stock AD9363's 20 MHz analog bandwidth cannot validate a 36 MHz `26m667` signal.
The receiver must actually cover the full occupied bandwidth; no firmware unlock
is performed. `--upsample 2` interpolates saved IQ offline before LeanDVB when
native capture is below two samples/symbol (but still covers the signal). It
cannot repair aliasing, cropped bandwidth or dropped samples. Spectrum always
uses native IQ; `decode.cfile` holds the interpolated copy.

## UHD

The default backend remains UHD. Use a Python with the driver for the firmware
actually loaded (`--uhd-python` overrides the Radioconda default). Pluto firmware
uses libiio; stock UHD availability alone does not establish E200 compatibility.
One RX channel, internal 40 MHz master clock, sc16 wire format, manual gain and
LO lock are checked. `5m` uses 10 MS/s / 10 MHz bandwidth; `10m` uses 20 MS/s /
20 MHz. The default antenna is `RX2`; select the connected port with `--antenna`.

```sh
python3 examples/over-the-air/check.py notes/ota-uhd-first \
  --args addr=192.168.1.10 --modcod 7 --rate 5m --rx-gain 0 --seconds 30 --key
```

VCM uses this same verifier: `--pattern 4,7,11 --hold 4` or
`--pattern 4,11,13,17 --hold 1` at `--rate 5m`. Mixed modulation selects axes
automatically; VCM at `10m` is refused before device access.
[Measured results and exact rerun commands](../../docs/vcm.md).

## Laptop-driven live VCM

One keyed run of this demo switched QPSK 1/2 to 3/4 to 9/10 and back to 1/2, and LeanDVB
recovered 938 frames in four segments (179, 124, 104 and 531), 4,355,900 bytes exact and
contiguous across every switch ([report](../../docs/measurements/ota/live-vcm-5m/report.json)).

With both devices in the closed box and your own callsign stored, run from the
repository root (new evidence directory each time):

```sh
python3 examples/over-the-air/live.py notes/live-vcm \
  --backend libiio --uri ip:192.168.1.10 --port /dev/cu.usbmodem1101 \
  --receiver notes/leansdr/src/apps/leandvb \
  --mhz 2420 --rate 5m --sample-rate 10000000 --rx-gain 30 --gain 4 \
  --seconds 30 --schedule 0:4,3:7,6:11,9:4 --key
```

Add `--plan` and omit `--key` for a device-free preview. The script reuses the
receive-only worker and its fail-closed TX inhibits. It calls the USB sender to
stream unique GCS bytes at 2 Mb/s, inserting PV type-3 controls during one keyed
run: QPSK 1/2 → 3/4 → 9/10 → 1/2. Times are seconds after USB-ready plus the
opening Morse duration; control 0 is sent immediately. Firmware applies changes
at frame boundaries. The stored callsign must leave three seconds after the last
change and before the trailing ID; otherwise the script refuses before keying.
This demo uses 5 Msym/s streaming to capture every change in one run.

`report.json` lists `segments` in receive order: MODCOD, frame count, recovered
frame indices, exact sent-payload byte range, and host control time/byte offset.
These are host request times, not SDR sample timestamps. Every segment needs at
least eight frames. All full recovered BBHEADERs, PLS modes and payload bytes must
match one contiguous slice of `sent.bin`, including across every switch. A switch
must follow its control within one old-mode frame of payload. Leading acquisition
and trailing partial-frame loss are allowed; interior loss, repeats, corruption,
missing/reordered runs and rejected controls fail. USB-limited dummy frames are
expected and reported separately from payload continuity. FEC checks reuse the
pattern verifier.

`sender.json` retains confirmed writes and control logs; `input.bin` is the offered
source, and `sent.bin` its confirmed sent prefix. Capture/readbacks, raw IQ,
BBFRAMEs, decoder logs, board counters, revision and cleanup acknowledgements stay
in the evidence directory. Replay needs no devices and leaves originals unchanged:

```sh
python3 examples/over-the-air/live.py notes/live-vcm-audit --recheck notes/live-vcm
python3 tests/test_live_vcm.py  # device-free; add --receiver "$RX" for C → LeanDVB proof
```

## Offline replay and evidence

An existing radio-off USB dump needs its original frame count, callsign and saved
sample rate **after decimation**. This opens no devices:

```sh
python3 examples/over-the-air/check.py notes/ota-usb-check \
  --dry-run /path/to/saved-7-5m.iq --dump-rate 20000000 \
  --rate 5m --modcod 7 --frames 16 --callsign N0CALL
```

N0CALL is a radio-off / contained fixture, never an assigned operating callsign.
Both RF backends use the same `analyze()` / `host/decode.py` path: frame lock,
complete FEC diagnostics, requested MODCOD, at least eight nonempty BB frames,
and **every recovered payload byte** matching one frame-aligned contiguous slice
of `decode.payload()`, including callsign headers. Leading acquisition loss and
trailing partial frames are allowed; corrupt bytes or an interior missing frame
fail. Reference length comes from the board's `frames_encoded`; ESP_OK, synthetic
input, healthy playback and zero S3 underruns are required.

`report.json` retains exact-byte results, decoded frames, LDPC/BCH corrections and
failures, locked MER and carrier offset; missing telemetry is `null`. Corrections
are not measured channel BER. `spectrum.png` is a Hann PSD in digital dBFS/Hz with
nominal RRC edges, not calibrated RF power or an emission mask. The libiio worker records sc16 first with four 1 Mi-sample device buffers
(419 ms at 10 MS/s), then converts after RX cleanup. Conversion cannot stall
capture refills. This buffers scheduling jitter; it does not prove continuity.
`capture.cfile` contains cf32_le IQ (80 MB/s at 10 MS/s). A 30 s streaming run has a 42 s safety
limit: allow 5.04 GB peak disk at 10 MS/s (raw plus converted IQ), plus optional interpolation. A 16 M
sample burst saves 128 MB. `capture.json`, `iio.log` / `uhd.log`, `board.jsonl`,
`settings.json`, `plan.json`, `revision.json`, `leandvb.log` and `payload.bin`
retain settings, readbacks and evidence. Failures exit nonzero and preserve files.

Saved VCM BBFRAMEs/logs can be audited without IQ or devices. The original
reports stay unchanged; an interior gap remains a failure, with exact segments
and missing frame indices reported:

```sh
python3 examples/over-the-air/check.py notes/vcm-mixed-audit --recheck notes/vcm-mixed-ota
```

Device-free tests: `python3 tests/test_ota.py`. All host tests:
`for test in tests/test_*.py; do python3 "$test" || exit 1; done`.
