# Bench setup

## Board

ESP32-S3 rev 0.2 with 8 MB embedded octal PSRAM and 16 MB flash, two 240 MHz cores (read by
`python3 host/board.py info` and esptool). Any S3 with 8 MB PSRAM and an antenna connector
should work. The firmware talks over the chip's own USB port (USB Serial/JTAG).

## Reset quirk on macOS

On macOS an RTS reset through the S3's USB port left GPIO0 low, so the chip restarted into its
bootloader (strap register 0x23, force-download flag 0). `host/board.py --reset` restarts the
firmware with esptool's watchdog reset instead, and opens the port without toggling the reset
lines. `idf.py flash` is unaffected.

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
