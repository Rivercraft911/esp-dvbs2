# Pi sender

Native PigeonVision `pv-capture` works directly with `spiinput pv`. Keep its
existing `spi` config: `/dev/spidev0.0`, 20 MHz, READY on RP1 GPIO25. No bridge
or change to pv-capture is needed. Start a fresh S3 receiver run first; every
sender begins at sequence zero. See [wiring and coordinator check](../../docs/spi-input.md).

For a file or ffmpeg stdin, install Python `spidev` and libgpiod v2 bindings
(`gpiod>=2,<3`). Enable SPI0 and grant access to SPI and the RP1 GPIO chip.
Use `gpioinfo` to identify that chip; it may be `/dev/gpiochip4` instead of 0.

```sh
python3 examples/pi-sender/send.py --vector
# 1332 bytes, CRC 0x51be328d, no hardware opened
python3 examples/pi-sender/send.py demo.ts --gpiochip /dev/gpiochip4
ffmpeg -re -i demo.ts -c copy -muxrate 9000000 -f mpegts pipe:1 |
  python3 examples/pi-sender/send.py - --gpiochip /dev/gpiochip4
```

The script sends raw 1,332-byte messages, mode 0, MSB first, at 20 MHz by default.
`--hz 1000000` is useful for initial wiring checks. File input defaults to a
9 Mb/s offered ceiling (`--mbps`); READY can reduce the actual rate. It sends
1..7 complete 188-byte packets, accumulates short stdin reads and rejects partial
TS tails or bad sync. No TS null packets are added. READY low for 2 seconds stops
it. Failed or short transfers are never retried. MISO is unused.

At exit it prints JSON counts and CRC chain to stderr. Compare `messages`,
`ts_packets`, `payload_bytes`, `crc_chain`, `next_seq` with the S3's `spi` fields
after stopping the sender and allowing several seconds for input to drain.
The S3 run must outlast the sender. A successful SPI write is not an ACK.
At 9 Mb/s, dummy DVB-S2 frames between data frames are expected.

The implementation is independent. Protocol and vector were checked against
PigeonVision's Python `spi_transport.py`, native `pv/spi_protocol.hpp`, and the
[RP2350 contract](https://github.com/Rivercraft911/RP2350_IQ_Benchmark/blob/main/docs/pv-spi-spec.md).
