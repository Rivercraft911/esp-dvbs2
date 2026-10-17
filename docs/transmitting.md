# Transmitting

Start with the [radio-off self-check](../examples/self-check/README.md). Key the radio only
with the operator's authorization, a valid amateur licence and an assigned callsign.
Use a closed shielded setup, a dummy load, or coax through at least 30 dB attenuation
into a receiver. A shielded box is not proof of zero leakage. Do not attach an antenna
for an initial bench check.

The console accepts integer center frequencies 2390..2450 MHz. That range is a hardware
limit, not authorization. Check your jurisdiction's band allocation, licence privileges
and permitted emissions. The full occupied bandwidth, plus the station-ID tone, must fit
inside your authorized band. Gain 0..17 is an uncalibrated vendor index, not dBm.

## Board and USB

The measured board is an ESP32-S3 rev 0.2 with two 240 MHz cores, 8 MB octal PSRAM
and 16 MB flash. Use ESP-IDF v6.1 and `firmware/sdkconfig.defaults`; the optional
`sdkconfig.xiao` selects the XIAO SPI pins.

| Port on the measured board | Purpose |
|---|---|
| Native USB, 303a:1001, `/dev/cu.usbmodem101` | Commands, results and flashing |
| CH343 UART, 1a86:55d3, `/dev/cu.usbmodem5981...` | Hardware EN reset |

One session owns the board. `host/board.py` locks the native port and checks existing
native/UART users before reset. On macOS, resetting through native RTS can enter the
bootloader. `--reset` uses the bridge's EN reset, or esptool's watchdog reset when no bridge
is present, then opens native USB without a reset transition.

```sh
. ~/.espressif/tools/activate_idf_v6.1.sh >/dev/null 2>&1
(cd firmware && idf.py build flash)
python3 host/board.py info --reset
```

Flashing itself does not start a transmission. The firmware waits for a command.

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

Every nonempty synthetic payload has a 40-byte callsign header. To inspect ID without RF
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
dvb MHz MODCOD [5m|10m|20m|26m667|33m333] seconds [gain] [peak]
```

`tx` signals are `tone`, `tone1`..`tone511`, `two`, `neg`, `low`, `ionly`, `qonly`,
`zero`, `dci`, `dcq`. The default tone is +1 MHz at 40 MS/s, amplitude 256 DAC LSB;
`low` is amplitude 64; `two` adds equal +1 and +3 MHz tones of amplitude 128 each.
Frequencies double at 80 MS/s. `tone128` is the previously measured linear-drive range.
`dvb` defaults to peak 128, gain 0, and requires at least 16 and at most 508 DAC LSB peak.
[Rate and MODCOD choices](how-it-works.md#rates).

The automatic transmit-filter selector is retained. Register readbacks do not establish
filter flatness, radiated power or emission-mask compliance. Historical keyed records in
[measurements.md](measurements.md) establish timing only. Wideband RF decoding, spectrum,
sample continuity and measured station ID are still to do.

## Payload input

[spi-input.md](spi-input.md) describes the SPI input and its board wiring. That guide and
the SPI implementation are maintained separately for PigeonVision's pv-capture messages.
Its historical experiment-tool links refer to the
[development archive](https://github.com/Rivercraft911/esp-dvbs2/tree/archive/dev-2026-10-17).
