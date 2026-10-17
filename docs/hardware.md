# Bench setup

The board is an ESP32-S3 rev 0.2 with 8 MB octal PSRAM, 16 MB flash and two
240 MHz cores. Commands/results use native USB Serial/JTAG. Timing and heap
claims require this board; host/QEMU clocks do not establish them.

| USB-C port | Function |
|---|---|
| Native USB, 303a:1001, `/dev/cu.usbmodem101` | Commands, results and flashing |
| CH343 UART, 1a86:55d3, `/dev/cu.usbmodem5981...` | Hardware EN resets |

An RTS reset on the native macOS port enters the bootloader. Use
`python3 host/board.py info --reset`: it resets through CH343, or falls back to
esptool's watchdog reset, then opens native USB without toggling reset lines.
The UART stub had checksum failures; native `idf.py flash` works.

## Radio-off checks

One session owns the board. The coordinator builds/flashes the committed
revision, then runs `host/check.sh`. Its `--plan` prints gates without devices.
The check uses dumps, paced bank fills and NULL-mode Wi-Fi start/stop only.
[Exact acceptance criteria](bank-geometry.md#evidence-and-acceptance),
[fill/CRC layouts](bank-fill.md), [encoder](encoder.md).

The tight heap baseline is 22,351 B free with Wi-Fi up for `wifimem 17 5m`.
The probe allocates the transmitter, station ID and worker, starts Wi-Fi, reads
heap, then stops before retuning, gain, tones, keying or bank grants.
`wifi_free_up` and `wifi_min_free` are different quantities; both are checked.

## Station identification

The operator supplies their assigned callsign and any required indicators.
The syntax check does not establish authorization. `callsign <CALL>` accepts
3..23 ASCII characters, normalizes uppercase and stores in NVS; `callsign`
reads it and `callsign clear` removes it. Invalid input preserves the setting;
a storage fault disables RF. Missing callsign rejects `tx`, `dvb` and `wifimem`
before startup. Dumps, paces and benchmarks remain available.

The implementation sends CW at 20 WPM before and after successful keyed runs
and at 540 s intervals of the nominal sample timeline. RF duration includes
IDs and remains limited to 60 s. Too-short requests fail before worker creation.
Paced runs instead add IDs to the requested payload duration, so a long radio-off
run can test periodic ID. A fatal fault stops immediately; closing ID cannot
be guaranteed. The receiver must reacquire after CW.

PARIS has 50 dot units: `60 s / (20 * 50) = 60 ms/dot`. Edges use a 5 ms raised
cosine. The 40-sample tone is +1 MHz at 40 MS/s and +2 MHz at 80 MS/s; ID sample
counts scale with DAC rate to retain duration. N0CALL takes 5.22 s per ID.
[Measured samples and model limits](../results/2026-10-16-station-id/README.md).

Every nonempty payload starts with a 40 B ID header. `host/onair.py --callsign
CALL` includes that in its bit-exact comparison. Dummy frames carry no payload.
`iddump [CALL] [40|80]` emits ID samples at 10 kS/s without changing NVS or
starting Wi-Fi. Check the deliberately aliased envelope with
`host/morse.py id.iq --rate 10000 --tone 0 --expect CALL`.

The implementation was designed around [47 CFR §97.119(a)–(c)](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-D/part-97/subpart-B/section-97.119),
checked 2026-10-16, and [ITU-R M.1677-1, Annex 1, Part I §§1–2](https://www.itu.int/dms_pubrec/itu-r/rec/m/r-rec-m.1677-1-200910-i!!pdf-e.pdf).
RF identification, startup transients and boundary clicks remain unqualified
until received and measured.

## RF path and capture

Only the coordinator operates hardware during cleanup. For later authorized
transmission, use the U.FL connector, a U.FL-to-SMA pigtail, at least 30 dB of
attenuation and coax to the receiver. Record the actual path and whether it
radiates. Unmeasured cable loss is not attenuation credit. The firmware's
2390..2450 MHz range and gain indices are not proof of band authorization or
calibrated power. Account for occupied bandwidth and the ID offset.

The E200 is an AD9363 unit running firmware in AD9364 mode. Its advertised
56 MHz/61.44 MS/s configuration exceeds the AD9363's guaranteed 20 MHz
bandwidth. Measure its passband before trusting wide captures or the full-rate
PRBS tag. Rejected acquisition is inconclusive without that qualification.

Print an RF capture plan first, substituting the actual path and local builds:

```sh
python3 host/bench.py dvb --topic cable --attenuation 20 10 \
  --cable 'U.FL-SMA -> 20 dB -> 10 dB -> coax -> E200 RX1; no antennas' \
  --radiates no --dry-run --libiio /path/to/libiio --receiver /path/to/leandvb
```

After the operator authorizes that finite run, replace `--dry-run` with
`--authorize-tx`. The tool records path, firmware, read-backs, hashes, logs,
receiver results and captures in a fresh results directory. It does not flash
or reset. Ctrl-C cannot stop the firmware's timed RF run; keep the cable
connected until it ends. Tone diagnostics use `host/bench.py tx` and the same
path controls. `host/capture.py` analyzes tone/image/leak and gap timing.

Offline receiver analysis opens no devices:

```sh
python3 host/onair.py capture.iq --rate 4000000 --scale 2048 \
  --auto-shift --mer --callsign CALL --receiver /path/to/leandvb
```

For E200 int16 captures use scale 2048; board dumps use 512. Modulation error
ratio (MER) fits carrier/clock offsets and compares symbol power with residual
error. It is qualified against synthetic signals and saved dumps, not an RF
channel measurement. A complete buffer does not establish sample continuity.
[Radio model and measurements](bank-geometry.md).
