# Pi sender

Send PV-SPI v1 messages from a Linux SBC to the S3's standard raw SPI input.
See the [message format, wiring, READY rule and commands](../../docs/spi-input.md).
PigeonVision `pv-capture` works unchanged with `spiinput on` (TS): SPI0/CE0,
20 MHz, READY on GPIO25. No prefix or bridge is needed.

Enable SPI0 and install Python `spidev` and libgpiod v2 bindings (`gpiod>=2,<3`).
Use `gpioinfo` to identify the GPIO chip carrying READY; on a CM5 it may be
`/dev/gpiochip4`. The sender needs access to that chip and `/dev/spidev0.0`.

First, on the S3 console:

```text
spipins devkit
spiinput on
dvbpace 13 5m 120
```

Then on the SBC, start a TS sender (file or ffmpeg stdin):

```sh
python3 examples/pi-sender/send.py --vector
# 1332 bytes, CRC 0x51be328d; no hardware opened
python3 examples/pi-sender/send.py demo.ts --gpiochip /dev/gpiochip4
ffmpeg -re -i demo.ts -c copy -muxrate 9000000 -f mpegts pipe:1 |
  python3 examples/pi-sender/send.py - --gpiochip /dev/gpiochip4
```

For arbitrary bytes, select `spiinput on gcs` before starting the S3 run, then:

```sh
python3 examples/pi-sender/send.py bytes.bin --kind gcs --gpiochip /dev/gpiochip4
```

The sender uses mode 0, MSB first, one raw 1,332-byte message per CS at 20 MHz.
`--hz 1000000` selects a slower clock. `--mbps` sets the offered payload ceiling
(default 9 Mb/s); READY can reduce the actual rate. It accumulates short reads,
zero pads the final message, and rejects partial TS packets or bad sync.
GCS accepts any nonempty batch of up to 1,316 bytes. No TS null packets are added.
After every CS release it waits at least 10 µs, then samples READY immediately
before the transfer. READY low for two seconds stops the sender. Failed or short
transfers are never retried. MISO is unused; this script sends raw SPI only.

Stop the sender several seconds before the S3 run ends. Compare its stderr JSON
with S3 `spi`: `messages`, `payload_bytes`, `next_seq`, and, for TS, `ts_packets`
and `crc_chain`. The latter chains each accepted TS message's four wire CRC bytes
through `zlib.crc32`, initially zero. Require zero receiver protocol errors and
sender timeouts/uncertain writes. A completed SPI transfer is not an ACK.
Restart the S3 run for each new sender (sequence starts at zero).
At 9 Mb/s the encoder may insert DVB-S2 dummy frames; payload bytes remain in order.
