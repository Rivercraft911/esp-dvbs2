# Per-frame MODCOD switching

VCM (variable coding and modulation) changes the MODCOD, the modulation and code rate pair,
from one frame to the next. It is opt-in and off after every reset. Fixed-mode waveforms, CRC
fixtures and bank schedules are unchanged. Normal frames, pilots and roll-off 0.35 stay
fixed; MODCOD 4 to 17 can change before the next frame's payload is read. The synthetic or
external byte stream continues across changes. VCM runs at 1 Msym/s (QPSK only) and
5 Msym/s, with GCS or synthetic payloads. TS input, short frames and `--fec` with live USB
controls are refused.

| Rate | VCM path | Evidence / limit |
|---|---|---|
| 1 Msym/s | QPSK phase table | Exact, contiguous E200 OTA code-rate switching |
| 5 Msym/s | Same-modulation phase table | QPSK 4/7/11 hold 4: 2,938 branch / 2,932 merged OTA frames, exact and contiguous |
| 5 Msym/s | Mixed QPSK/8PSK, automatic five-level axes | 4/11/13/17 every frame: 3,523 branch / 3,522 merged OTA frames, 21,055,600 bytes exact and contiguous |
| 10 Msym/s and higher | Refused before encoder/radio startup | Not qualified on the board; mixed 10m exceeded the encoder budget on an earlier branch |

The OTA (over-the-air) runs went from the S3 to an ANTSDR E200, both inside a closed Faraday
box, on 2026-10-18. Branch results are from the tags `archive/exp/vcm` (`b893fa8`) and
`archive/exp/modswap` (`154692d`); merged results are from `9956748`, through the public
libiio verifier. One report is copied here: the branch QPSK run, 2,938 frames and
16,960,052 bytes exact, median MER (modulation error ratio) of the frame headers 20.5 dB
([report](measurements/ota/vcm-qpsk-5m/report.json)). The other counts have no copied
report. On `9956748` the self-check passed 28 of 30: MODCOD 13 at 26m667 exceeded the fill
ceiling, and the VCM pace had 18 late banks (86,243 cycles against 85,608).

Board timing since the placement work below is clean. Mixed VCM at 5 Msym/s filled its
slowest bank in 68,264 cycles against the 85,608-cycle deadline
([tx-pins](../results/2026-10-23-tx-pins/README.md)). On 27259c5 the self-check passed all 30
checks, its VCM dump, decode and pace included
([self-check](../results/2026-10-23-tx-self-check/README.md)).

The axis table is 10,240 bytes instead of the 65,536-byte mixed phase table at
5 Msym/s. Both preserve the existing integer pulse rounding and symbol history.
VCM hot code has its own IRAM section, so it keeps the SRAM instruction fetches it had on the
modswap branch. Every fill kernel keeps its measured fetch
alignment and the 8PSK lock keeps its cache index; linker checks guard placement
([records](../results/2026-10-23-tx-pins/README.md)). Phase profiling stays enabled
for VCM pace. Same-modulation runs keep their smaller phase tables. Mixed 5m automatically
uses axes. Run-memory failures report stage, requested bytes
and the run-memory levels before cleanup.

Console patterns use `vcm <frames per entry> <MODCOD ...>` (1 to 16 entries,
hold 1 to 65,535 frames); the run must start at the first MODCOD. For example:

```sh
python3 host/board.py 'vcm 1 4 11 13 17'
python3 host/board.py 'dvbdump 4 5m 32 128 1' --save notes/vcm.iq
python3 host/board.py 'vcm off'
python3 host/decode.py notes/vcm.iq --rate 40000000 --symbol-rate 5000000 \
  --modcod 4 --stream 32 --pattern 4,11,13,17 --hold 1 --callsign N0CALL \
  --receiver "$RX"
```

`vcm on` accepts PV type-3 controls within the starting modulation; `vcm mixed`
allows both modulations at 5m. The control payload is one MODCOD byte, carried
in the existing CRC-protected, sequenced PV message on SPI or USB. Controls do
not consume payload bytes. A console pattern disables live controls. Unsupported
changes increment `control_rejected`. `vcm off` restores fixed mode.

For USB, put the desired MODCOD in a text file and use
`examples/usb-send/send.py send FILE --kind gcs --modcod 4 --rate 5m --control MODCOD_FILE`.
Edit the file during the run; add `--mixed` to allow QPSK/8PSK. This defaults to
a radio-off pace. The sender checks scope before sending each update. Driven from a laptop
with timed controls (`--schedule`), one keyed run switched QPSK 1/2 to 3/4 to 9/10 and back to
1/2 over the air: 938
frames and 4,355,900 bytes exact and contiguous across every switch
([report](measurements/ota/live-vcm-5m/report.json),
[how to run it](../examples/over-the-air/README.md#laptop-driven-live-vcm)).

## Checks to run after flashing

The self-check exercises VCM on every run. Build and flash `firmware/tx` first, store
N0CALL for the radio-off fixtures, close other serial clients and use fresh output
directories. See the [self-check](../examples/self-check/README.md),
[SPI check](spi-input.md#test-your-sender) and [USB check](../examples/usb-send/README.md).

```sh
export PORT=/dev/cu.usbmodem1101
export RX="$PWD/notes/leansdr/src/apps/leandvb"
python3 examples/self-check/check.py notes/vcm-layout-self --port "$PORT" --receiver "$RX"
PORT="$PORT" RX="$RX" python3 examples/usb-send/check.py notes/vcm-layout-usb
python3 host/board.py --port "$PORT" 'vcm 1 4 11 13 17'
python3 host/board.py --port "$PORT" 'dvbpace 4 5m 30'
python3 host/board.py --port "$PORT" 'vcm off'
```

The self-check adds a 32-frame radio-off 4/11/13/17 dump and independent decode:
CRC and sample geometry must agree, at least 30 contiguous exact full BBFRAMEs
must include every requested MODCOD, and FEC must need no corrections.
A 5 s VCM pace checks bank deadlines and that every run-memory arena stays within its size;
the final `info` must report no heap call after boot.
Existing golden CRCs and paced timing gates remain intact.

## Contained OTA runs

These are the commands behind the two 5 Msym/s OTA rows above. They have not been rerun
since the placement work. Follow the [RF setup](../examples/over-the-air/README.md). Both
devices stay in the closed Faraday box (or use the documented attenuated cable path). The
operator stores their own callsign and must verify it, the frequency, containment and gains
before `--key`. The keyed window, 1 to 60 s in `check.py` (the firmware allows up to 540 s),
includes both Morse IDs. Ctrl-C cannot cancel firmware already keyed.

```sh
export IIO_URI=ip:192.168.1.10
python3 examples/over-the-air/check.py notes/vcm-layout-qpsk-ota \
  --backend libiio --uri "$IIO_URI" --port "$PORT" --receiver "$RX" \
  --mhz 2420 --rate 5m --pattern 4,7,11 --hold 4 --sample-rate 10000000 \
  --rx-gain 30 --gain 4 --peak 128 --seconds 30 --key
python3 examples/over-the-air/check.py notes/vcm-layout-mixed-ota \
  --backend libiio --uri "$IIO_URI" --port "$PORT" --receiver "$RX" \
  --mhz 2420 --rate 5m --pattern 4,11,13,17 --hold 1 --sample-rate 10000000 \
  --rx-gain 30 --gain 4 --peak 128 --seconds 30 --key
```

Add `--plan` and omit `--key` for a device-free preview. `check.py` is the single
OTA verifier for fixed modes and VCM, using the receive-only libiio backend
and TX-inhibit readbacks. `--pattern` selects its initial MODCOD automatically.
It requires a healthy board run with zero underruns and late banks, then matches
PLS order, full BBHEADERs and every payload byte against one contiguous host-model
interval. Every pattern mode must be recovered. Acquisition/EOF loss is allowed;
an interior gap, repeat or corrupt frame fails. FEC totals and the recovered
interval's diagnostics are separate; corrections are reported, not called BER.
VCM setup and cleanup acknowledgements, raw IQ, receiver logs and reports are kept.
