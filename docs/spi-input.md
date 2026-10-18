# SPI input

Send bytes to the ESP32-S3; it frames, encodes and transmits DVB-S2 on the chip.
**PV-SPI v1 over raw single-line SPI is the standard input.** PigeonVision's
`pv-capture` works unchanged: no command, address or dummy prefix.
TS carries MPEG transport packets; GCS carries arbitrary continuous bytes.
Choose one stream type per run. TS adaptation is performed by the encoder;
GCS bytes pass unchanged. No application header is added.

## Message (both wires)

Every CS assertion carries exactly **1,332 message bytes**. Multibyte fields
are little endian; bit order on the wire is MSB first.

| Byte offset | Size | Value |
|---|---:|---|
| 0 | 2 | ASCII `PV` (`50 56`) |
| 2 | 1 | Version `1` |
| 3 | 1 | Type: `0` NOP, `1` TS, `2` GCS |
| 4 | 2 | Sequence; increment for every message, including NOP; wrap at 65536 |
| 6 | 2 | Valid payload length |
| 8 | 4 | Reserved, zero |
| 12 | 1,316 | Payload followed by zero padding |
| 1328 | 4 | CRC-32 of bytes 0..1327, including padding |

NOP has length 0; TS has 1..7 whole 188-byte packets, each starting `0x47`;
GCS has 1..1,316 bytes. CRC is reflected IEEE CRC-32 (`zlib.crc32`), polynomial
`0xedb88320`, initial/final XOR `0xffffffff`; `123456789` checks to `0xcbf43926`.
Check vector: TS sequence 1, one packet `47 01 00 10` followed by bytes 0..183,
then zero padding, has CRC **`0x51be328d`** (wire bytes `8d 32 be 51`).

Bad headers, lengths, CRCs, TS sync or stream types discard the message.
Duplicates/backward sequences are discarded; forward gaps are counted (less
than 32768 missing messages). Start a fresh receiver run when restarting a sender.

## Wire and READY

- Mode 0, active-low CS, MSB first. Raw SPI uses D0/MOSI only, up to **20 MHz**;
  no prefix, exactly 1,332 bytes per CS. MISO is unused.
- Hold CS setup and hold for at least two SCK periods. After CS rises, wait
  **at least 10 µs**, then sample READY immediately before the next CS assertion.
- Send only when READY is high; finish a started message even if it falls.
  Never queue transfers using an earlier READY sample. READY includes DMA
  readiness and buffer credit. Low means pause; v1 has no ACK or retry.
- **Quad option:** up to 40 MHz; send command `0x23`, address `0x00` on D0,
  then **8 dummy clocks**, then the same 1,332-byte PV message on D0..D3.
  High nibble first, bit 3 on D3; command/address stay single-line. Same READY rule.

## Pins

Use 3.3 V logic and common ground.

| Signal | DevKit GPIO | XIAO GPIO / label | CM5 IO-board 40-pin header |
|---|---:|---|---|
| SCK | 12 | 7 / D8 | GPIO11, pin 23 |
| CS | 10 | 1 / D0 | GPIO8 / CE0, pin 24 |
| D0 / MOSI | 11 | 2 / D1 | GPIO10, pin 19 |
| D1 (quad only) | 13 | 4 / D3 | Unconnected for raw SPI |
| D2 (quad only) | 14 | 5 / D4 | Unconnected |
| D3 (quad only) | 9 | 6 / D5 | Unconnected |
| READY (S3 output) | 8 | 8 / D9 | GPIO25 input, pin 22 |
| GND | GND | GND | Pin 20 or 25 |

Raw SPI needs only SCK, CS, MOSI, READY and GND; leave MISO and D1..D3 disconnected.

## Commands

| Console command | Effect |
|---|---|
| `spipins devkit` / `spipins xiao` | Choose the pin preset |
| `spipins SCK CS D0 D1 D2 D3 READY` | Override all seven GPIOs; each must be distinct |
| `spiinput on [ts\|gcs]` | Standard raw input; TS by default |
| `spiinput quad [ts\|gcs]` | Quad input, same messages; TS by default |
| `spiinput off` | Restore synthetic input |
| `spiinput on [ts\|gcs] loop Mb/s` | On-chip sender for the next run; also works with `quad` |
| `spirx 3000 on loop 9` | Three-second on-chip receive/pattern check, radio off |

Input selection applies to the next `dvbdump`, `dvbpace`, `wifimem` or `dvb`;
it does not start reception or RF. Old `pv`, `ts`, `gcs` selectors remain aliases
for `on ts`, `quad ts`, `quad gcs`. Pin/input choices last until changed or reset.

## Rates

Ideal payload ceiling: `R = 8*1316 / ((H + 8*1332/L)/f_SCK + 10e-6)` bits/s,
where raw has `(H,L)=(0,1)` and quad `(24,4)`; READY and sender overhead lower it.
Raw at 20 MHz: **19.396 Mb/s** ideal. Quad at 40 MHz: **136.37 Mb/s** ideal;
**46.6 Mb/s** was verified standalone, not as an end-to-end encoder rate.
Encoder payload ceiling: `R = 8*DATA_FIELD_bytes*symbol_rate/PLFRAME_symbols`.
8PSK 2/3 needs 9.678 Mb/s at 5 Msym/s, 19.357 Mb/s at 10 Msym/s.
When input runs slower, the S3 emits DVB-S2 dummy frames and resumes data in order.

## Test your sender

On the S3 console, select pins, `spiinput on` (or `on gcs`), then
`dvbpace 13 5m 120` for a radio-off encoder run. Start the sender afterwards
and stop it several seconds before the S3 run ends so accepted input can drain.
On a Linux SBC, enable SPI0, install `spidev` and `gpiod>=2,<3`, and use `gpioinfo`
to find the READY GPIO chip. The [reference sender](../examples/pi-sender/README.md)
accepts files or stdin, handles short reads and sends one message per transfer:

```sh
python3 examples/pi-sender/send.py --vector
python3 examples/pi-sender/send.py demo.ts --gpiochip /dev/gpiochip4
python3 examples/pi-sender/send.py bytes.bin --kind gcs --gpiochip /dev/gpiochip4
```

Compare sender/S3 `messages`, `payload_bytes`, `next_seq`; for TS also compare
`ts_packets`, `crc_chain` (CRC-32 chained over each accepted TS message's four CRC
bytes, initially zero). Require zero protocol/sequence/overrun errors and sender
timeouts/uncertain writes. A completed write is not an acknowledgement.

On-chip loopback needs no jumpers: SPI3 drives SPI2 through the same pads.
Disconnect external SPI devices before using `loop`. The coordinator can check
raw TS/GCS, quad, decoded payloads and synthetic waveform CRCs in about two minutes
on an already flashed board with callsign `N0CALL` (radio off; logs under `notes/`):

```sh
PORT=/dev/cu.usbmodem101 PINS=devkit RX=/path/to/leandvb bash host/spi-board.sh
```
