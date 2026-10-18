# Transmitting

Start with the [radio-off self-check](../examples/self-check/README.md). Key the radio only
with the operator's authorization, a valid amateur licence and an assigned callsign.
For the [E200 RF check](../examples/over-the-air/README.md), use a closed Faraday box
with both devices inside, or coax through at least 30 dB attenuation into the receiver.
Antennas are allowed only inside the closed box. A shielded box is not proof of zero
leakage. Never run this check with an exposed antenna or an unattenuated receiver path.

The console accepts integer center frequencies 2390..2450 MHz. That range is a hardware
limit, not authorization. Check your jurisdiction's band allocation, licence privileges
and permitted emissions. The full occupied bandwidth, plus the station-ID tone, must fit
inside your authorized band. Gain 0..17 is an uncalibrated vendor index, not dBm.
In the US, consult [47 CFR §97.301](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-D/part-97/subpart-D/section-97.301)
for licence/band privileges, [§97.303](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-D/part-97/subpart-D/section-97.303)
for sharing restrictions, [§97.305](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-D/part-97/subpart-D/section-97.305)
for permitted emissions and [§97.307](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-D/part-97/subpart-D/section-97.307)
for emission standards. Check applicable local rules and band plans before keying;
the coordinator's `--key` and stored-callsign checks do not establish legal authority.

## Board and USB

Use ESP-IDF v6.1; [install it with Espressif's ESP32-S3 guide](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/get-started/index.html).

| Board | PSRAM | Flash | Configuration |
|---|---|---|---|
| Measured S3 rev 0.2, N16R8 | 8 MB octal | 16 MB | `sdkconfig.defaults` |
| Optional XIAO ESP32-S3 | 8 MB octal | 8 MB | defaults + `sdkconfig.xiao`; changes flash size and SPI pins |

Apply the XIAO overlay to a fresh, separate configuration and build directory:

```sh
(. ~/.espressif/tools/activate_idf_v6.1.sh && cd firmware && \
  idf.py -B ../notes/build-xiao -D SDKCONFIG="$PWD/../notes/sdkconfig.xiao" \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.xiao' build)
```

Use new output paths for the first overlay build; this preserves an existing `firmware/sdkconfig`.
The activation path shown is the tested macOS EIM path; use your own activation/export script
if installed elsewhere. The defaults already select `esp32s3`.

| Port on the measured board | Purpose |
|---|---|
| Native USB, 303a:1001, `/dev/cu.usbmodem101` | Commands, results and flashing |
| CH343 UART, 1a86:55d3, `/dev/cu.usbmodem5981...` | Hardware EN reset |

One session owns the board. `host/board.py` locks the native port and checks existing
native/UART users before reset. On macOS, resetting through native RTS can enter the
bootloader. `--reset` uses the bridge's EN reset, or esptool's watchdog reset when no bridge
is present, then opens native USB without a reset transition.

```sh
(. ~/.espressif/tools/activate_idf_v6.1.sh && cd firmware && idf.py build flash)
python3 host/board.py info --reset
```

Run host tools in the [separate host shell](../README.md#quick-start). Flashing itself does
not start a transmission. The firmware waits for a command. For a build without a board,
omit `flash`.

## Console

`host/board.py` sends one command and prints the JSON results. `--save` writes sample dumps.

| Command | Purpose |
|---|---|
| `info` | Firmware revision, clocks, memory and stored callsign |
| `callsign [CALL\|clear]` | Read, set or clear the station ID |
| `dvbdump MODCOD [rate] frames [peak] [decimation] [stall_ms]` | Samples over USB, radio off |
| `dvbpace MODCOD [rate] seconds [peak] [stall_ms]` | Live encoder and bank deadlines, no playback |
| `spiinput on\|quad\|off [ts\|gcs]` | Select [SPI payload input](spi-input.md) for the next run |
| `vcm off\|on\|mixed\|N MODCOD...` | Opt-in [GCS rate switching](vcm.md); repeat each mode for N frames |
| `spipins devkit\|xiao` | Select the SPI pin preset |
| `spirx ms on\|quad [ts\|gcs] [loop [Mb/s [fault]]]` | Radio-off SPI receive diagnostic |
| `spisend ms on\|quad [ts\|gcs] [MHz [Mb/s [fault]]]` | SPI test sender on a second S3; no RF |
| `wifimem MODCOD [rate]` | Radio-off Wi-Fi startup and heap check |
| `iddump [CALL] [40\|80]` | Morse ID samples at 10 kS/s, radio off |
| `tx MHz signal seconds [gain] [40\|80]` | Transmit a test tone |
| `dvb MHz MODCOD [rate] seconds [gain] [peak]` | Transmit DVB-S2 |

MODCOD selects modulation and code rate: QPSK 4..11, 8PSK 12..17. The
[rate table](how-it-works.md#rates) gives the exact choices. QPSK defaults to 1 Msym/s.
Both modulations accept `5m`, `10m`, `20m` and `26m667`; `33m333` is QPSK only.
Dumps are little-endian int16 I/Q with full scale 512. Golden CRCs use peak 128 and N0CALL.
SPI argument ranges, custom pins and fault/replay options: [diagnostics](spi-input.md#diagnostics).

All three IQ helpers (`host/decode.py`, `host/morse.py`, `examples/live-video/receive.py`)
accept these `--format` aliases. Each pair is I followed by Q, little endian.

| Sample type | Aliases | Bytes per complex sample |
|---|---|---:|
| Signed int16, S3 dumps (full scale 512) | `i16`, `s16` | 4 |
| Float32 | `cf32`, `f32` | 8 |

```sh
python3 host/board.py "dvbdump 15 20m 16 128 1" --save fast.iq
python3 host/decode.py fast.iq --rate 80000000 --symbol-rate 20000000 \
  --modcod 15 --stream 16 --callsign N0CALL --receiver /path/to/leandvb
python3 host/board.py "dvbpace 11 26m667 10" --timeout 120
```

Completion records include `main_stack_bytes` and `main_stack_min_free`: the
main task's configured stack size and minimum unused bytes since boot. The
`wifimem` result also reports the minimum before setup, after encoder startup
and after the Wi-Fi probe. A reboot clears the VCM pattern; confirm `vcm:true`
in the result before treating a dump or pace as a VCM check.

## Station identification

`callsign CALL` stores 3..23 ASCII characters in nonvolatile storage (NVS), normalizing
uppercase. `callsign` reads the setting; `callsign clear` removes it. Use the callsign
assigned to your station and any required indicators. A syntax check cannot verify a licence.
Missing or invalid storage disables transmission.

Successful keyed runs include 20 WPM Morse at the start and end; the implementation also
schedules ID every nine minutes. Console transmissions are limited to 60 s, including both
IDs. Too-short requests fail before startup. A fatal fault stops immediately, so a closing
ID cannot be guaranteed. The receiver must reacquire after Morse.

The US identification rule requires identification at the end of a communication and at
least every ten minutes, and limits automatic CW identification to 20 WPM. See
[47 CFR §97.119(a)–(c)](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-D/part-97/subpart-B/section-97.119)
(checked 2026-10-17). The operator remains responsible for the applicable rules.
Morse timing follows [ITU-R M.1677-1, Annex 1](https://www.itu.int/dms_pubrec/itu-r/rec/m/r-rec-m.1677-1-200910-i!!pdf-e.pdf).
At 20 WPM, `60 s/(20*50) = 60 ms/dot`; edges use a 5 ms raised cosine.
The ID tone is +1 MHz from center at 40 MS/s, +2 MHz at 80 MS/s.
N0CALL, used only for contained tests, takes 5.22 s per ID.

Every nonempty synthetic payload has a 40-byte callsign header. External SPI/USB payloads
receive no added callsign header; their bytes are preserved, with normal DVB-S2 TS sync
handling. Start/end Morse ID still runs independently. To inspect ID without RF
or a storage change, substitute your callsign:

```sh
python3 host/board.py "iddump CALL 80" --save id.iq
python3 host/morse.py id.iq --rate 10000 --tone 0 --expect CALL
```

The 10 kS/s dump deliberately aliases the ID tone to zero so the helper can read its
envelope. [Archived station-ID measurements](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-16-station-id/README.md).
RF identification and startup/boundary transients still need external measurement.

## Keyed commands

Connect the contained path first. With an external RF connector, use a U.FL-to-SMA pigtail,
attenuators and coax to a receiver or dummy load. Record actual attenuation and the complete
path; do not count unknown cable loss. Verify the receiver's input-power limits and
bandwidth before connecting it. Keep the path connected until a timed run finishes;
stopping the host process does not stop the firmware's timed transmission.

After setting your assigned callsign, the keyed syntax is:

```
tx MHz signal seconds [gain] [40|80]
dvb MHz MODCOD [5m|10m|20m|26m667|33m333] seconds [gain] [peak] [--fixture] [--phy-solo] [--fused]
```

`tx` signals are `tone`, `tone1`..`tone511`, `two`, `neg`, `low`, `ionly`, `qonly`,
`zero`, `dci`, `dcq`. The default tone is +1 MHz at 40 MS/s, amplitude 256 DAC LSB;
`low` is amplitude 64; `two` adds equal +1 and +3 MHz tones of amplitude 128 each.
Frequencies double at 80 MS/s. `tone128` is the previously measured linear-drive range.
`dvb` defaults to peak 128, gain 0, and requires at least 16 and at most 508 DAC LSB peak.
[Rate and MODCOD choices](how-it-works.md#rates).

At QPSK 33m333 or 8PSK 26m667, `--fixture` repeats the prepared synthetic frame
used by `dvbpace`, with the same payload copy. Payload ingress must be off.
The record includes `prepared_fixture:true`. This separates encoder/copy time
from generating the default contiguous xorshift stream.

`dvb` and `dvbpace` accept two independent experimental options. Without them,
the existing renderer, Wi-Fi lifetime, queue sizing and payload stream stay selected.
`--phy-solo` starts and calibrates Wi-Fi, holds the PHY independently, then stops and
deinitializes Wi-Fi before playback. It releases that hold on every exit. Solo paces
are limited to 60 seconds. **RF quality without Wi-Fi's periodic recalibration is
unverified**; use the [E200 A/B plan](../examples/over-the-air/README.md#phy-solo-ab).

At 8PSK `26m667`, `--fused` automatically tries six table reads instead of eight.
It preserves independent rounding and DAC words. The extra 12 KiB of tables displaces
24 static queue pages; one or two internal heap regions restore the full **54 pages**.
Admission happens after Wi-Fi setup or solo teardown, retaining the 20,000 B floor
plus 2,048 B guard. Insufficient or fragmented RAM falls back to the original static
queue, with `fused_tails:false`; no pages are dropped. `fused_requested`, `fused_tails`,
`queue_sizing` and `queue_layout` record the decision. The common 64 KiB store and
5 Msym/s self-check's 22,351 B heap floor are retained. The fused kernel has a separate
instruction-cache lock region; playback locks only the selected region. Default playback
retains main's 1,632-byte lock footprint, with no fused dispatch in its page loops.
R&D timing is motivation; this port still needs coordinator timing and RF checks.

The automatic transmit-filter selector is retained. Register readbacks do not establish
filter flatness, radiated power or emission-mask compliance. Historical keyed records in
[measurements.md](measurements.md) establish timing only. Wideband RF decoding, spectrum,
sample continuity and measured station ID are still to do.
The [contained RF coordinator](../examples/over-the-air/README.md) captures the E200
before keying and checks LeanDVB's recovered synthetic payload bytes. Its `--plan` and
`--dry-run` paths are device-free; only an explicit `--key` permits hardware access.
No successful RF decode is established by adding this workflow.

## Payload input

By default every run carries a synthetic test payload with the callsign header. `spiinput on`
takes real data over SPI for the next run instead; see [spi-input.md](spi-input.md).
