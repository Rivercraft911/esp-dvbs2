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

## Frequencies and licence

The first tests use 2390–2450 MHz, in the US 13 cm amateur allocation. Transmitting there needs
an amateur licence, and running the radio outside Wi-Fi channels means the module's Wi-Fi
certification does not cover it. Keep the antenna off and the signal in a cable until a licensed
operator is on the air.
