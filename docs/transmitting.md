# Transmitting

Start with the [radio-off self-check](../examples/self-check/README.md). Key the radio only
with the operator's authorization, a valid amateur licence and an assigned callsign.
For the [E200 RF check](../examples/over-the-air/README.md), use a closed Faraday box
with both devices inside, or coax through at least 30 dB attenuation into the receiver.
Antennas are allowed only inside the closed box. A shielded box is not proof of zero
leakage. Never run this check with an exposed antenna or an unattenuated receiver path.

The console accepts integer center frequencies 2000..2450 MHz for conducted research.
Reception has been checked at selected frequencies in that range; lock, power and signal
quality across the full range remain uncharacterized. Check your jurisdiction's band allocation, licence privileges
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
(. ~/.espressif/tools/activate_idf_v6.1.sh && cd firmware/tx && \
  idf.py -B ../../notes/build-xiao -D SDKCONFIG="$PWD/../../notes/sdkconfig.xiao" \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.xiao' build)
```

Use new output paths for the first overlay build; this preserves an existing `firmware/tx/sdkconfig`.
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
(. ~/.espressif/tools/activate_idf_v6.1.sh && cd firmware/tx && idf.py build flash)
python3 host/board.py info --reset
```

Run host tools in the [separate host shell](../README.md#quick-start). Flashing itself does
not start a transmission. The firmware waits for a command. For a build without a board,
omit `flash`.

## Console

`host/board.py` sends one command and prints the JSON results. `--save` writes sample dumps.

`rflo direct` is the default tuning path. Conducted tests at lower frequencies can also
select `rflo five-sixths`, which asks the PLL for six-fifths of the requested RF frequency
and enables the radio's five-sixths divider. Either command accepts an optional signed
offset in kHz, from −999 to 999; omitting it resets the offset to zero. The resulting
RF request must stay within 2000..2450 MHz.
Run records report `requested_rf_khz` and `requested_pll_khz`; measure the actual signal
with a receiver. Tuning selections last until changed or the board restarts.

| Command | Purpose |
|---|---|
| `info` | Firmware revision, clocks, memory and stored callsign |
| `callsign [CALL\|clear]` | Read, set or clear the station ID |
| `dvbdump MODCOD [rate] frames [peak] [decimation] [stall_ms] [--short]` | Samples over USB, radio off |
| `dvbpace MODCOD [rate] seconds [peak] [stall_ms] [--short] [--fixture]` | Live encoder and bank deadlines, no playback |
| `fecbench MODCOD [--short]` | BCH and LDPC cycles on one frame, radio off |
| `spiinput on\|quad\|off\|usb [ts\|gcs] [replay]` | Select [SPI or USB payload input](spi-input.md) for the next run |
| `vcm off\|on\|mixed\|N MODCOD...` | Opt-in [GCS rate switching](vcm.md); repeat each mode for N frames |
| `spipins devkit\|xiao` | Select the SPI pin preset |
| `spirx ms on\|quad [ts\|gcs]` | Radio-off SPI receive diagnostic |
| `spisend ms on\|quad [ts\|gcs] [MHz [Mb/s]]` | SPI test sender on a second S3; no RF |
| `iddump [CALL] [40\|80]` | Morse ID samples at 10 kS/s, radio off |
| `rflo direct\|five-sixths [kHz]` | Tuning path and offset for the next keyed run |
| `tx MHz signal seconds [gain] [40\|80]` | Transmit a test tone |
| `dvb MHz MODCOD [rate] seconds [gain] [peak] [--short] [--fixture]` | Transmit DVB-S2 |

MODCOD selects modulation and code rate: QPSK 4..11, 8PSK 12..17. The
[rate table](how-it-works.md#rates) gives the exact choices. QPSK defaults to 1 Msym/s.
`--short` selects 16,200-bit short frames: QPSK MODCOD 4..10 at the default 1 Msym/s or `5m`, and 8PSK MODCOD 12..16 (3/5 .. 8/9, no short 9/10) at `5m` (not VCM, `10m` or faster; SPI input untested). Each frame carries (Kbch - 80) / 8 payload bytes: 1,454 at MODCOD 7 and 1,454 at 8PSK MODCOD 14. A pilot-on short frame has 8,370 QPSK or 5,598 8PSK symbols.
Both modulations accept `5m`, `10m`, `20m` and `26m667`; `33m333` is QPSK only.
Dumps are little-endian int16 I/Q with full scale 512. Golden CRCs use peak 128 and N0CALL.
SPI argument ranges, custom pins and the replay option: [diagnostics](spi-input.md#diagnostics).

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
main task's configured stack size and minimum unused bytes since boot. A reboot clears the VCM pattern; confirm `vcm:true`
in the result before treating a dump or pace as a VCM check.

## Station identification

`callsign CALL` stores 3..23 ASCII characters in nonvolatile storage (NVS), normalizing
uppercase. `callsign` reads the setting; `callsign clear` removes it. Use the callsign
assigned to your station and any required indicators. A syntax check cannot verify a licence.
Missing or invalid storage disables transmission.

My station uses **KJ5SCH**. Substitute your assigned callsign when setting up your board:

```sh
python3 host/board.py "callsign KJ5SCH"
```

Successful keyed runs include 20 WPM Morse at the start and end; the implementation also
schedules ID every nine minutes. `dvb` runs are limited to 540 s and `tx` to 60 s,
including both IDs. Too-short requests fail before startup. A fatal fault stops immediately, so a closing
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
dvb MHz MODCOD [5m|10m|20m|26m667|33m333] seconds [gain] [peak] [--fixture]
```

`tx` signals are `tone`, `tone1`..`tone511`, `two`, `neg`, `low`, `ionly`, `qonly`,
`zero`, `dci`, `dcq`. The default tone is +1 MHz at 40 MS/s, amplitude 256 DAC LSB;
`low` is amplitude 64; `two` adds equal +1 and +3 MHz tones of amplitude 128 each.
Frequencies double at 80 MS/s. `tone128` is the previously measured linear-drive range.
`dvb` defaults to peak 128, gain 0, and requires at least 16 and at most 508 DAC LSB peak.
[Rate and MODCOD choices](how-it-works.md#rates).

For **80 MS/s modes** (`20m`, `26m667`, `33m333`), use **peak 48** as the starting point.
At QPSK 9/10, 20 Msym/s, peak 128 overdrives the radio and does not decode; peaks 32–64
decode, with 48 best at 14.2 dB MER. The earlier narrow-filter measurement was about
−3 dB at ±10 MHz and −20 dB by ±14 MHz. Main restores the wide setting after CW setup
for signals above 20 MHz occupied bandwidth; B200 captures now decode 36/40 MHz modes.
The [RF records](../results/2026-10-20-wide-filter/README.md) identify prepared payloads and receiver limits.

At QPSK 33m333 or 8PSK 26m667, `--fixture` repeats the prepared synthetic frame
used by `dvbpace --fixture`, with the same payload copy. Payload ingress must be off.
The record includes `prepared_fixture:true`. This separates encoder/copy time
from generating the default contiguous xorshift stream.

The transmitter never starts Wi-Fi. At boot it powers the radio, enables the PHY through its
Bluetooth modem bit, which runs the full calibration once, and keeps it enabled. No Wi-Fi
interrupt or task runs during playback. `--phy-solo`, which used to stop Wi-Fi before playback,
is still accepted and changes nothing. Into the S3 receiver at 1 Msym/s, PHY-solo keying gave 0 failed
frames in 103,453, against 2 in 51,791 with Wi-Fi up: Wi-Fi's activity caused rare short
disturbances of the transmitted signal ([records](../results/2026-10-23-tx-phy-solo/README.md)).

The automatic transmit-filter selector is retained. Register readbacks do not establish
filter flatness, radiated power or emission-mask compliance. Historical keyed records in
[measurements.md](measurements.md) establish timing; the new RF section records decoded
QPSK and VCM payloads, a spectrum and the earlier narrow-filter limit. The
[wide-filter checks](../results/2026-10-20-wide-filter/README.md) decode 36/40 MHz bursts;
sustained wideband video remains unqualified.
The [contained RF coordinator](../examples/over-the-air/README.md) captures the E200
before keying and checks LeanDVB's recovered synthetic payload bytes. Its `--plan` and
`--dry-run` paths are device-free; only an explicit `--key` permits hardware access.
Its `--backend libiio` run recovered 2,937 byte-exact QPSK 3/4 frames at 5 Msym/s.

## Payload input

By default every run carries a synthetic test payload with the callsign header. `spiinput on`
takes real data over SPI for the next run instead; see [spi-input.md](spi-input.md).
