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

## Frequencies and licence

The first tests use 2390–2450 MHz, in the US 13 cm amateur allocation. Transmitting there needs
an amateur licence, and running the radio outside Wi-Fi channels means the module's Wi-Fi
certification does not cover it. Keep the antenna off and the signal in a cable until a licensed
operator is on the air.
