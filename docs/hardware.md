# Bench setup

## Board

ESP32-S3 rev 0.2 with 8 MB embedded octal PSRAM and 16 MB flash, two 240 MHz cores (read by
`python3 host/board.py info` and esptool). Any S3 with 8 MB PSRAM and an antenna connector
should work. The firmware talks over the chip's own USB port (USB Serial/JTAG).

The board has two USB-C ports:

| Port | Chip | Used for |
|---|---|---|
| Native USB (`USB JTAG_serial debug unit`, 303a:1001) | S3 USB Serial/JTAG | commands, results, flashing, payload tests |
| UART (`USB Single Serial`, 1a86:55d3) | WCH CH343 bridge on UART0, with the usual EN/GPIO0 auto-reset circuit | hardware resets |

## Resets

On macOS an RTS reset through the native port left GPIO0 low, so the chip restarted into its
bootloader (strap register 0x23, force-download flag 0). `host/board.py --reset` therefore pulls
EN low through the CH343 port when it is plugged in, and otherwise falls back to esptool's
watchdog reset. It opens the native port without toggling the reset lines. Over the CH343 port,
esptool's RAM stub upload failed with a checksum error at the default settings; `--no-stub`
works. `idf.py flash` on the native port is unaffected.

## Receiving the signal

- Take RF from the board's U.FL connector with a U.FL-to-SMA pigtail.
- Put at least 30 dB of attenuation between the board and the receiver. The radio can put out
  around +20 dBm; esp-sdr-zephyr's S3-to-S3 tests used 30 dB.
- An AD9363-based SDR (E200) records the signal for offline analysis and decoding.

## Radio experiment

Only the coordinator flashes or operates the board for `exp/radio`. Connect the attenuation
before sending a valid `tx` command: Wi-Fi PHY calibration precedes the timed DAC playback.
The command is `tx <integer MHz> <tone|two|tag> <1..60 seconds> [gain]`, with centers restricted
to 2390–2450 MHz and vendor gain steps 0–17, default 0. Gain is not calibrated in dBm.
`tone` is +1 MHz relative to the requested center; `two` is +1 and +3 MHz. Keep the actual
signal within the licensed band when choosing the center.

Analyze active capture interiors with `python3 host/capture.py file.iq --format i16
--rate 10000000 --signal tone` (`f32` also works). Power is dBFS, not dBm. The `tag` phase
sequence checks individual 40 MS/s samples and needs at least 50 MS/s complex capture and a
qualified 40 MHz RF passband. The AD9363 is specified for [up to 20 MHz channel bandwidth](https://www.analog.com/en/products/ad9363.html),
so it is not qualified for this full-rate continuity test. Rejected tag acquisition is
inconclusive. No RF result is claimed until the coordinator measures it.

## Frequencies and licence

The first tests use 2390–2450 MHz, in the US 13 cm amateur allocation. Transmitting there needs
an amateur licence, and running the radio outside Wi-Fi channels means the module's Wi-Fi
certification does not cover it. Keep the antenna off and the signal in a cable until a licensed
operator is on the air.

## Monday checklist

1. Give this session exclusive ownership of the board and SDR. Connect the board U.FL to an
   SMA pigtail, then **30–40 dB of attenuators**, then coax to E200 **RX1**. Leave antennas off.
   Record each attenuator in path order and describe the whole cable path. Do not count an
   unmeasured cable loss as attenuation. Explicitly record whether anything radiates.
2. Use the already flashed `dvb` firmware. Its command sends QPSK at 1 Msym/s, with eight
   repeated normal frames, pilots and roll-off 0.35 (`firmware/main/dvbtx.c:3`,
   `firmware/main/dvbtx.c:9`, `firmware/main/dvbtx.c:31`, `firmware/main/dvbtx.c:32`).
   Start with MODCOD 4, peak 128 and board gain 0. Peak 128 is the initial setting suggested
   by the recorded amplitude sweep, not a calibrated RF power
   ([radio results, Drive level](../results/2026-10-16-radio/README.md#drive-level)).
3. Set E200 LO 2420 MHz, sample rate 4 MS/s, bandwidth 2 MHz, manual gain 0 dB, port
   `A_BALANCED`. These are starting settings for the cabled test. The ADI driver documents
   RX1 and the separate RX/TX LOs under
   [General attribute naming convention](https://analogdevicesinc.github.io/linux/drivers/iio-transceiver/ad9361/#general-attribute-naming-convention).
   `host/sdr.py` powers down `altvoltage1`, verifies all eight requested attributes, and
   requires scan channels I/Q at indices 0/1 with `le:S12/16>>0`. Any mismatch stops the run.
4. From the checkout being tested, first print the plan. Replace **20 10** and the cable
   description with the actual installed path. Use the framework Python if IDF is activated:

   ```sh
   /Library/Frameworks/Python.framework/Versions/3.13/bin/python3 host/bench.py dvb \
     --topic monday-cable --attenuation 20 10 \
     --cable 'U.FL-SMA pigtail -> 20 dB -> 10 dB -> SMA coax -> E200 RX1; no antennas' \
     --radiates no --dry-run \
     --libiio /Users/river/Documents/stanford/SSI/esp-dvbs2/notes/libiio \
     --receiver /Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb
   ```

   After checking the path and frequency under your amateur licence, run the **same command**
   with `--dry-run` replaced by `--authorize-tx`. Omitting the RF-path options prompts on a
   terminal; a noninteractive run refuses missing fields. The authorization applies to that
   one timed run. Neither command resets or flashes the board.
5. Defaults send `dvb 2420 4 10 0 128`, wait 2 s after starting `board.py`, then request
   **one 4,000,000-sample buffer**, 1 s and 16,000,000 bytes of int16 I/Q. The generated
   capture command uses `iio_readdev -b 4000000 -s 4000000` (libiio v0.25
   [buffer creation and refill](https://github.com/analogdevicesinc/libiio/blob/v0.25/tests/iio_readdev.c#L398)).
   Settings are read back again immediately before acquisition. The firmware has no RF-start
   acknowledgement; the 2 s settle is an assumption to check on Monday. The tool refuses
   capture if the board has already ended or the planned TX time remaining is too short.
6. Check the final JSON status and both `results/<local-date>-monday-cable/runs.jsonl` and
   `captures.jsonl`. They retain the RF path, board `info` and timed-command records,
   host revision/tool hashes, read-backs, libiio versions, capture times/hash, and analysis.
   Keep the local `.iq`, `.iq.json`, board log and receiver log with the run. A successful
   DVB run requires known-symbol MER recovery, LeanDVB frame lock and a complete eight-frame
   payload match, plus a successful board result without late fills or fatal deadlines.
   MER is modulation error ratio, the fitted reference-symbol power divided by residual
   error power. Carrier offset and a constant clock error are removed; phase noise and
   channel distortion remain. This estimator is qualified with synthetic signals and saved
   radio-off dumps, not RF captures. A single-buffer file length does not prove RF continuity.
   On errors, the tool waits for the finite board command and records its outcome. Ctrl-C
   cannot directly stop firmware RF; keep the cable connected until the timed run has ended.

For an initial tone instead, use `tx` with the same RF-path and libiio options and add
`--bandwidth 4000000`. The default `tone128` and gain 0 command lasts 10 s; `host/capture.py`
reports tone/image/leak and envelope/phase statistics with full scale 2048, in dBFS, not dBm.
For offline reanalysis (no devices are opened):

```sh
python3 host/onair.py results/<date>-monday-cable/<run-id>.iq \
  --rate 4000000 --scale 2048 --auto-shift --mer \
  --receiver /Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb
python3 host/onair.py capture.iq --rate 4000000 --scale 2048 --auto-shift --mer-only
```
