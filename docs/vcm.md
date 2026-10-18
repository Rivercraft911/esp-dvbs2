# Per-frame MODCOD switching

VCM is opt-in and resets off. Fixed-mode waveforms, CRC fixtures and bank schedules
are unchanged. Normal frames, pilots and roll-off 0.35 stay fixed; MODCOD 4–17
can change before the next frame's payload is read. The PRNG / external byte
stream continues across changes. GCS and synthetic payloads work; TS adaptation
and `--fec` with live USB controls are not supported.

| Rate | VCM path | Evidence / limit |
|---|---|---|
| 1 Msym/s | QPSK phase table | Exact, contiguous E200 OTA code-rate switching |
| 5 Msym/s | Same-modulation phase table | QPSK 4/7/11: 2,938 branch / 2,935 merged OTA frames, exact and contiguous |
| 5 Msym/s | Mixed QPSK/8PSK, automatic five-level axes | 4/11/13/17 every frame: 3,523 exact, contiguous OTA frames, 21 MB; about 77 KB internal heap free |
| 10 Msym/s | Same modulation, phase table | Host-model coverage; board timing needs checking |
| 10 Msym/s mixed | Refused before encoder/radio startup | Earlier branch decoded, but exceeded encoder budget and inserted dummies / lost data |
| Higher rates | Refused | VCM queues and timing not qualified |

OTA measurements above came from `exp/vcm` (`b893fa8`) and `exp/modswap`
(`154692d`), S3 → ANTSDR E200, both inside a closed Faraday box. They are branch
measurements. On merged `709976c`, SPI and USB passed and QPSK recovered
2,935 exact frames / 16,948,004 bytes. Mixed VCM recovered 3,505 individually
exact frames / 20,954,034 bytes, but lost model frames 1051–1069 between two
exact segments; nine in-interval FEC failures correctly failed the check. Axis
selection, headers and payload generation matched. Without saved IQ, the loss
cannot be localized between RF and receive transport. The capture worker now
matches the earlier helper's larger buffers and defers conversion until RX stops.
Default timing placement was restored after the 26m667 pace regression. Both
changes need the radio-off and contained reruns below; host tests cannot prove timing.

The axis table is 10,240 bytes instead of the 65,536-byte mixed phase table at
5 Msym/s. Both preserve the existing integer pulse rounding and symbol history. VCM hot
code has its own optional cache lock; fixed ring/encoder layouts and the default
1,632-byte lock stay independent.
Same-modulation runs keep their smaller phase tables. Mixed 5m automatically
uses axes; `vcm axes on` also selects axes explicitly at 5m, and `vcm axes off`
restores automatic selection. Allocation failures report stage, requested bytes,
free heap and largest blocks before cleanup.

Console patterns use `vcm <frames per entry> <MODCOD ...>` (1–16 entries,
hold 1–65535); the run must start at the first MODCOD. For example:

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
a radio-off pace. The sender checks scope before sending each update.

## Checks to run after flashing

These commands are for the operator; they have not been run against hardware
for this merge. Use fresh output directories. Build/flash separately, store
N0CALL for radio-off fixtures, close serial clients, and disconnect SPI jumpers
before the on-chip loopback check. See the [self-check](../examples/self-check/README.md),
[SPI check](spi-input.md) and [USB check](../examples/usb-send/README.md).

```sh
export PORT=/dev/cu.usbmodem1101
export RX=/Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb
python3 examples/self-check/check.py notes/vcm-self --port "$PORT" --receiver "$RX"
PORT="$PORT" RX="$RX" bash host/spi-board.sh notes/vcm-spi
PORT="$PORT" RX="$RX" python3 examples/usb-send/check.py notes/vcm-usb
```

The self-check adds a 32-frame radio-off 4/11/13/17 dump and independent decode:
CRC and sample geometry must agree, at least 30 contiguous exact full BBFRAMEs
must include every requested MODCOD, and FEC must need no corrections.
A 5 s VCM pace checks bank deadlines and the 20 KB Wi-Fi heap floor.
Existing golden CRCs and paced timing gates remain intact.

## Two contained OTA reruns

Follow the [RF setup](../examples/over-the-air/README.md). Both devices stay in
the closed Faraday box (or use the documented attenuated cable path). The operator
must verify the stored callsign, frequency, containment and gains before `--key`.
The keyed window includes both Morse IDs. Ctrl-C cannot cancel firmware already keyed.

```sh
export IIO_URI=ip:192.168.1.10
python3 examples/over-the-air/check.py notes/vcm-qpsk-ota \
  --backend libiio --uri "$IIO_URI" --port "$PORT" --receiver "$RX" \
  --mhz 2420 --rate 5m --pattern 4,7,11 --hold 4 --sample-rate 10000000 \
  --rx-gain 30 --gain 4 --peak 128 --seconds 30 --key
python3 examples/over-the-air/check.py notes/vcm-mixed-ota \
  --backend libiio --uri "$IIO_URI" --port "$PORT" --receiver "$RX" \
  --mhz 2420 --rate 5m --pattern 4,11,13,17 --hold 1 --sample-rate 10000000 \
  --rx-gain 30 --gain 4 --peak 128 --seconds 30 --key
```

Add `--plan` and omit `--key` for a device-free preview. `check.py` is the single
OTA verifier for fixed modes and VCM, using main's receive-only libiio backend
and TX-inhibit readbacks. `--pattern` selects its initial MODCOD automatically.
It requires a healthy board run with zero underruns and late banks, then matches
PLS order, full BBHEADERs and every payload byte against one contiguous host-model
interval. Every pattern mode must be recovered. Acquisition/EOF loss is allowed;
an interior gap, repeat or corrupt frame fails. FEC totals and the recovered
interval's diagnostics are separate; corrections are reported, not called BER.
VCM setup and cleanup acknowledgements, raw IQ, receiver logs and reports are kept.
