# Payload over the S3's USB port

Firmware `cd30c89`, ESP32-S3 at 240 MHz, macOS host, the same USB Serial/JTAG port used for
commands. `python3 host/board.py "usbrx 4000000" --send 4000000`, three runs, records in
`runs.jsonl`: the board reads straight from the USB driver (8 KiB receive buffer) and times the
first to the last byte.

| Run | Bytes | Rate | CRC-32 matches host |
|---|---:|---:|---|
| 1 | 4,000,000 | 4.62 Mbit/s | yes |
| 2 | 4,000,000 | 4.62 Mbit/s | yes |
| 3 | 4,000,000 | 4.61 Mbit/s | yes |

Enough for the 1–3 Mbit/s baseline, about half of the 9.68 Mbit/s video mode. The port is USB
full speed (12 Mbit/s on the wire); this path moves 64-byte packets through the driver's interrupt
handler. The board also computed a bitwise CRC of every byte, roughly 10% of one core at this
rate, which does not explain the ceiling. Video payload needs another path, most likely an SPI
slave with DMA.
