# Contained RF check

This coordinator closes the next evidence gap: E200 IQ capture → independent LeanDVB
decode → exact synthetic payload comparison. **No successful RF decode is claimed yet.**
USB replay and host-generated waveforms test the workflow only.

Read [Transmitting](../../docs/transmitting.md) first. Use a **closed Faraday box with both
devices inside**, or **coax with attenuators** (at least 30 dB initially). Antennas belong
inside the closed box only. Record the path, attenuation, antenna/connector and gain.
Verify the E200 input limits; the S3 gain index is not calibrated power. A shielded box
does not establish zero leakage, licence authority, or emission-mask compliance.

The operator must hold the required licence, store their assigned callsign beforehand,
choose an authorized frequency/emission, and keep the complete signal inside their band.
The default center is 2420 MHz; this is an explicit bench setting, not band permission.
Keep the contained path connected until the firmware's timed run finishes. Ctrl-C stops
the coordinator/capture; it **cannot stop an already keyed firmware run**.

## RF settings

Normal frames, pilots, RRC roll-off 0.35; all three MODCODs work at either rate:

| MODCOD | Mode | Symbol rate | Nominal occupied bandwidth | UHD sample rate | RX bandwidth | Master clock / decimation |
|---|---|---|---|---|---|---|
| 7 | QPSK 3/4, first check | 5 Msym/s | 6.75 MHz | 10 MS/s | 10 MHz | 40 MHz / 4 |
| 7 | QPSK 3/4, demo | 10 Msym/s | 13.5 MHz | 20 MS/s | 20 MHz | 40 MHz / 2 |
| 13 | 8PSK 2/3 | 10 Msym/s | 13.5 MHz | 20 MS/s | 20 MHz | 40 MHz / 2 |
| 11 | QPSK 9/10 | 10 Msym/s | 13.5 MHz | 20 MS/s | 20 MHz | 40 MHz / 2 |

These choices use one RX channel and integer UHD decimation, at least two samples per
symbol, and an analog bandwidth within the AD9363's 20 MHz limit. The master clock is
the RFIC/UHD clock, **not** the 40 MHz reference oscillator. The chip's digital sample
rate limit and its analog bandwidth limit are different. See the
[AD9363 specifications](https://www.analog.com/en/products/ad9363.html),
[ADI sample-rate specifications](https://wiki.analog.com/university/tools/pluto/devs/specs), and
[E200 clock/streamer limits in the vendor driver](https://github.com/MicroPhase/antsdr_uhd/blob/master/host/lib/usrp/ant/ant_impl.cpp).
Do not request a second channel at a 40 MHz master clock. The vendor driver halves
its maximum clock for two channels. No 20 Msym/s mode is offered here.

Wire samples are sc16 (40 or 80 MB/s before Ethernet overhead); disk IQ is cf32_le
(80 or 160 MB/s). Use gigabit Ethernet, fast local storage and several GB free.
A 30 s run has a 42 s capture safety limit: approximately 3.36 GB at 5 Msym/s or
6.72 GB at 10 Msym/s. Normal capture stops one second after the board reports done.

## Operator commands

Use a host Python with `numpy scipy matplotlib pyserial`. Radioconda's Python is the
default capture worker; the coordinator uses the normal host Python (Radioconda here
does not include pyserial). The worker's UHD installation must
support the **E200 firmware actually loaded**: stock UHD availability alone does not
prove the vendor E200 transport is installed. See the
[vendor UHD setup](https://antsdr-docs.microphase.cn/en/latest/device_and_usage_manual/ANTSDR_E_Series_Module/ANTSDR_E200_Reference_Manual/AntsdrE200_UHD.html).
Set `--uhd-python` to another compatible Python if needed. No automatic discovery,
firmware loading, board reset, or callsign writes occur. Select the physically connected
receive port (`--antenna RX2` by default).

From the repository root, set `E200_IP` to the owner's configured device address.
`RX` defaults to the existing LeanDVB build at
`/Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb`.

```sh
export E200_IP=192.168.1.10  # replace with the actual E200 address
export RX=/Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb

# Read-only plan: no imports of board/UHD, no devices, no output files.
python3 examples/over-the-air/check.py --args "addr=$E200_IP" --rate 5m --modcod 7 --plan

# Operator executes only after closing the box / connecting the attenuated path.
python3 examples/over-the-air/check.py notes/ota-first-5m \
  --args "addr=${E200_IP:?set E200_IP}" --mhz 2420 --modcod 7 --rate 5m \
  --seconds 30 --gain 0 --peak 128 --key
```

The new output directory must not exist. Specify `--port` when native USB is elsewhere;
the coordinator uses that exact path and the `host/board.py` ownership lock, without
reset or port discovery. `info` must return a valid stored callsign. Optional `--callsign`
cross-checks it. A short keyed window that leaves less than two seconds of DVB after
both Morse IDs is refused. `spiinput off` selects synthetic input for this check; it
also disables USB ingress. This is a volatile input selection, not a callsign change.

The worker sets internal clock, manual RX gain (default 0 dB), antenna, center frequency,
sample rate and bandwidth. It checks actual readbacks and LO lock, then receives one
second of clean IQ before the coordinator sends `dvb MHz MODCOD rate seconds gain peak`.
The keyed seconds **include both IDs**. Capture continues through board startup and
the keyed window plus one second afterward; a finite safety limit bounds disk use.
Any sample-rate/master-clock/bandwidth coercion, missing IQ, overflow, metadata error,
or failed capture prevents a pass. A failure detected before keying prevents `dvb`.
After keying, the firmware retains responsibility for its timed shutdown.

Then repeat with fresh directories and `--rate 10m --modcod 7`, `--rate 10m --modcod 13`,
and `--rate 10m --modcod 11`. If acquisition fails, inspect spectrum and logs before
changing manual RX gain or RF settings; gain 0 on either device is not a power guarantee.

## PHY-solo A/B

Wi-Fi teardown removes periodic recalibration. CPU/heap records cannot establish
RF quality. After the radio-off self-check and paired `dvbpace` runs, the coordinator
runs this A/B **only inside the closed Faraday box**, keeping the path, supply,
attenuation, RX gain, peak and callsign fixed. Use fresh output directories. Repeat
in reverse order to distinguish warmup/drift from the option; the labels below record
both orders. Use the same `E200_IP`, `RX` and compatible UHD Python as above.

```sh
for mc in 13 15 17; do
  for arm in wifi-a solo-a solo-b wifi-b; do
    opt=''; case "$arm" in solo-*) opt='--phy-solo';; esac
    python3 examples/over-the-air/check.py "notes/ota-phy-$mc-$arm" \
      --args "addr=${E200_IP:?set E200_IP}" --receiver "${RX:?set RX}" \
      --mhz 2420 --modcod "$mc" --rate 10m --seconds 30 --gain 0 --peak 128 \
      $opt --key || exit 1
  done
done
```

Compare exact payload recovery, frame-lock continuity, decoded frame count, LDPC/BCH
corrections/failures, locked MER, carrier offset and PSD. Require the existing exact-byte
pass in every arm, complete/error-free captures, and solo records showing Wi-Fi absent,
PHY users 2, both cores' Wi-Fi IRQ routes zero before setup, then PHY users 0 after
cleanup. Investigate a repeated solo-only loss of lock, MER degradation, correction
increase, carrier drift or new spectral feature before accepting the option.

This E200 workflow uses `10m` (13.5 MHz occupied) for all three 8PSK modes. Its configured
20 MHz analog bandwidth cannot validate the full **36 MHz** `26m667` emission, and the
fused renderer runs only at that higher rate. A passing E200 A/B therefore tests the
PHY lifetime at 10 Msym/s; full-rate RF quality and fused-path RF validation remain
pending a receiver covering the complete occupied bandwidth. Do not infer those results
from the 26m667 keyed timing runs or from a narrow cropped capture.

## Offline USB replay

Use an **existing** radio-off USB dump and its exact metadata. This command does not
collect a new dump or open any device:

```sh
python3 examples/over-the-air/check.py notes/ota-usb-check \
  --dry-run /path/to/saved-7-5m.iq --dump-rate 20000000 \
  --rate 5m --modcod 7 --frames 16 --callsign N0CALL
```

`--dump-rate` is the saved sample rate **after decimation**, not the DAC clock.
For example, QPSK 5m with the default dump decimation 2 is 20 MS/s; a decimation-1
8PSK 5m dump is 40 MS/s. `--frames` must be the dump's original frame count.
N0CALL is a contained/radio-off fixture, never an assigned operating callsign.
The same `analyze()` calls `host/decode.py` for RF and USB, with int16 conversion and
4096 receiver lookahead samples added only for USB, as in the existing dump checker.

## Evidence and pass criteria

`report.json` records decoded BB frames, LDPC bit flips/parity-check counts, BCH corrected
bits/failures, exact-byte comparison, locked MER (dB) and carrier offset (Hz), with
count/min/median/max/last for receiver telemetry. Missing telemetry is `null`, never zero.
These are receiver estimates; the raw fd-info/debug output is in `leandvb.log`. Carrier
offset uses LeanDVB's signed FREQ estimate relative to the capture center. LDPC flips
and BCH corrections are not a measured channel BER.

A pass needs frame lock, complete FEC diagnostics, the requested MODCOD, at least eight
nonempty BB frames, and **every recovered payload byte** matching one frame-aligned,
contiguous slice of `host/decode.payload()` including the callsign headers. Leading
acquisition loss and trailing partial frames are allowed; corrupt bytes or an interior
missing frame fail. RF reference length comes from the board's `frames_encoded`.
Nonzero FEC corrections are reported and allowed when the recovered bytes match.
The board must also report ESP_OK, the expected synthetic mode and zero underruns.

`spectrum.png` uses `host/plotstyle.py`: Hann PSD from evenly spaced windows across the
capture, including IDs and silence, in digital dBFS/Hz, with nominal RRC band edges.
It is not a calibrated RF-power or regulatory mask measurement. USB spectra exclude
the appended receiver lookahead. RF capture remains as `capture.cfile`; `capture.json`
holds readbacks/device identity/sample count/errors, and `uhd.log` preserves diagnostics.
`board.jsonl`, `settings.json`, `plan.json`, `revision.json` and `payload.bin` retain
the command, stored callsign, firmware revision, settings and decoded evidence.
Failures exit nonzero and keep available evidence. USB reports explicitly say `usb-dump`.

The new device-free checks run as `python3 tests/test_ota.py`; the normal host suite
remains `for test in tests/test_*.py; do python3 "$test" || exit 1; done`.

Offline validation on 2026-10-17: native firmware-stream fixtures, resampled to the
planned two samples/symbol, decoded in the existing LeanDVB build with exact payloads
and zero LDPC/BCH corrections. QPSK 3/4 at 5 and 10 Msym/s and QPSK 9/10 at 10 Msym/s
recovered 15/16 frames; 8PSK 2/3 at 10 Msym/s recovered 13/16. A historical USB dump
without callsign headers was rejected. These checks did not access hardware or RF.
