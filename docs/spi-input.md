# SPI input

Give the S3 bytes over SPI and it transmits them: framing, error correction, filler frames
when the input runs dry, pulse shaping and the radio all happen on the chip. Any board that
can drive SPI and read one GPIO can be the sender: a Raspberry Pi, another microcontroller,
an FPGA.

## Two ways to wire it

| | Single-line (standard) | Quad |
|---|---|---|
| Wires | SCK, CS, MOSI, READY, GND | Single-line plus D1..D3 |
| Clock | up to 20 MHz | up to 40 MHz |
| Ideal payload rate | 19.4 Mb/s | 136 Mb/s |
| Tested on the board | 9 and 12 Mb/s | 46.6 Mb/s |
| Sender needs | plain SPI writes | one command byte + one address byte + 8 dummy clocks |
| Select with | `spiinput on` | `spiinput quad` |

The table's tested rates use on-chip loopback between the S3's two SPI peripherals.
An external CM5 sender also delivered a 9 Mb/s camera mux at 16 MHz through the
[live RF link](../results/2026-10-19-live-rf/README.md). Ideal rates follow from the
message size and the gap between messages (below).

## What it carries

| Type | Payload per message | Use |
|---|---|---|
| TS | 1 to 7 whole 188-byte MPEG transport-stream packets | Video and audio from any TS source (ffmpeg, a camera pipeline) |
| GCS | 1 to 1,316 arbitrary bytes | Files, telemetry, anything else |
| NOP | none | Keep-alive; carries no data |
| Control | one MODCOD byte, 4..17 | [VCM](vcm.md), GCS only; applies at a frame boundary |

Choose TS or GCS for a run (`spiinput on ts` or `spiinput on gcs`). TS packets get the
standard DVB-S2 sync handling; GCS bytes pass through unchanged. When input is slower than
the mode, the S3 sends DVB-S2 dummy frames and resumes the data in order.

## Which wire for which mode

| Mode | Payload rate | Wire |
|---|---:|---|
| 8PSK 2/3, 5 Msym/s | 9.68 Mb/s | Single-line |
| QPSK 3/4, 10 Msym/s | 14.5 Mb/s | Single-line |
| 8PSK 2/3, 10 Msym/s | 19.4 Mb/s | Quad (single-line has no headroom) |
| QPSK 9/10, 26.67 Msym/s | 46.6 Mb/s | Quad, at its tested rate |
| 8PSK 5/6, 20 Msym/s | 48.4 Mb/s | Quad, above its tested rate |
| QPSK 9/10, 33.33 Msym/s (40 MHz) | 58.2 Mb/s | Quad, above its tested rate |

## How messages are framed

Every CS assertion carries exactly one **1,332-byte message**. Multibyte fields are little
endian; bits go out MSB first. The [reference sender](../examples/pi-sender/README.md)
implements this format (PV-SPI v1). The [video example](../examples/live-video/README.md)
can supply TS from a file or ffmpeg.

| Bytes | Field | Value |
|---|---|---|
| 0..1 | Magic | ASCII `PV` (`50 56`) |
| 2 | Version | `1` |
| 3 | Type | `0` NOP, `1` TS, `2` GCS, `3` MODCOD control |
| 4..5 | Sequence | +1 for every message, NOP included; wraps at 65536 |
| 6..7 | Length | Valid payload bytes |
| 8..11 | Reserved | zero |
| 12..1327 | Payload | data, then zero padding to 1,316 bytes |
| 1328..1331 | CRC-32 | over bytes 0..1327, padding included |

The CRC is the standard reflected IEEE CRC-32 (`zlib.crc32` in Python): polynomial
`0xedb88320`, initial and final XOR `0xffffffff`; `123456789` gives `0xcbf43926`.
Check vector: a TS message with sequence 1 and one packet `47 01 00 10` followed by bytes
0..183 has CRC **`0x51be328d`** (wire bytes `8d 32 be 51`).

The S3 drops any message with a bad header, length, CRC, TS sync byte or type, and any
duplicate or backward sequence number. Forward gaps are counted. Restart the S3 run when you
restart the sender, so sequences start fresh.

## Timing and READY

READY is an S3 output that says it has room for another message.

Eight 1,336-byte receive buffers use 10,688 bytes of DMA RAM, borrowed from
unused encoder regions when all eight fit, otherwise allocated from the heap.
The two-slot READY margin permits seven unread messages (9,212 payload bytes),
enough for a complete 5,370-byte BB payload while the encoder works.

- SPI mode 0, active-low CS, MSB first. MISO is unused.
- Hold CS for at least two SCK periods before the first and after the last clock.
- After CS rises, wait **at least 10 µs**, then read READY right before the next message.
- Send only while READY is high. Finish a message you started even if READY falls.
- Never send on an old READY reading. There is no acknowledgement or retry.
- Quad: send command `0x23` and address `0x00` on D0, then **8 dummy clocks**, then the same
  1,332-byte message on D0..D3, high nibble first, bit 3 on D3.

That is 8 command + 8 address + 8 dummy clocks, **24 preamble clocks total**.
It follows the [S3 TRM](https://www.espressif.com/sites/default/files/documentation/esp32-s3_technical_reference_manual_en.pdf),
§30.5.9, table 30.5-14 (Wr_DMA `0x23`); both the firmware
receiver and on-chip sender configure those same three 8-clock phases.

The ideal payload rate is one message per transfer plus the 10 µs gap:

```
R = 8*1316 / ((H + 8*1332/L) / f_SCK + 10e-6)    bit/s
single-line: H = 0,  L = 1   -> 19.4 Mb/s at 20 MHz
quad:        H = 24, L = 4   -> 136 Mb/s at 40 MHz
```

## Pins

3.3 V logic, common ground.

| Signal | DevKit GPIO | XIAO GPIO / label | Raspberry Pi CM5 IO board, 40-pin header |
|---|---:|---|---|
| SCK | 12 | 7 / D8 | GPIO11, pin 23 |
| CS | 10 | 1 / D0 | GPIO8 / CE0, pin 24 |
| D0 / MOSI | 11 | 2 / D1 | GPIO10, pin 19 |
| D1 (quad only) | 13 | 4 / D3 | not used |
| D2 (quad only) | 14 | 5 / D4 | not used |
| D3 (quad only) | 9 | 6 / D5 | not used |
| READY (S3 output) | 8 | 8 / D9 | GPIO25 input, pin 22 |
| GND | GND | GND | pin 20 or 25 |

## Commands

| Console command | Effect |
|---|---|
| `spipins devkit` / `spipins xiao` | Pin preset |
| `spipins SCK CS D0 D1 D2 D3 READY` | Any seven distinct GPIOs |
| `spiinput on [ts\|gcs]` | Single-line input for the next run; TS by default |
| `spiinput quad [ts\|gcs]` | Quad input for the next run |
| `spiinput off` | Back to the synthetic test payload |
| `spiinput usb [ts\|gcs]` | The same messages over the USB cable instead of SPI |
| `spiinput quad [ts\|gcs] replay` | In-memory sealed messages through the real parser, no transfers |
| `spirx ms on\|quad [ts\|gcs]` | Receive diagnostic from an external sender, radio off |
| `spisend ms on\|quad [ts\|gcs] [MHz [Mb/s]]` | Test sender on a second S3 |

Input selection applies to the next `dvbdump`, `dvbpace` or `dvb`; it does not
start reception or the radio. Settings last until changed or reset.

## Diagnostics

`spirx` and `spisend` accept **100..120000 ms**, TS by default.
`spisend` clocks at **1..20 MHz** single-line (default 20), or **1..40 MHz** quad
(default 40). Offered payload rate is **0..140 Mb/s**; zero means unpaced, still obeying
READY and the inter-message gap. `spirx` waits for an external sender. Corrupt, short,
long and lost messages are covered by the host model of the receiver (`tests/ingress_sim.c`).

`spiinput quad [ts\|gcs] replay` supplies an in-memory pattern without SPI transfers;
it is a diagnostic, not external input. `spipins SCK CS D0 D1 D2 D3 READY` accepts seven
distinct output-capable GPIOs; choose pins available on your board.

## Test your sender

On the S3, pick pins, run `spiinput on` (or `spiinput on gcs`), then start a radio-off run
such as `dvbpace 13 5m 120`. Start the sender after that and stop it a few seconds before the
run ends so the S3 can drain what it accepted.

The [reference sender](../examples/pi-sender/README.md) runs on a Linux board with SPI0
enabled, `spidev` and `gpiod>=2,<3` installed (`gpioinfo` finds the READY GPIO chip). It reads
a file or stdin and sends one message per transfer:

```sh
python3 examples/pi-sender/send.py --vector            # check the CRC vector, no hardware
python3 examples/pi-sender/send.py demo.ts --gpiochip /dev/gpiochip4
python3 examples/pi-sender/send.py bytes.bin --kind gcs --gpiochip /dev/gpiochip4
```

Afterwards the sender's and the S3's `messages`, `payload_bytes` and `next_seq` should match,
and for TS also `ts_packets` and `crc_chain` (a CRC-32 chained over each accepted message's
CRC bytes, starting from zero). Protocol, sequence and overrun error counts should be zero.

A second S3 running `spisend` is the quad-rate sender: wire its SPI pins to the receiving
S3's preset, run `spiinput quad` and a radio-off run on the receiver, then `spisend` on the
sender.

## Over USB instead

The same messages also work over the S3's USB cable, with no wiring at all. The
[examples/usb-send](../examples/usb-send/README.md) Python sender uses only `pyserial`
and defaults to radio-off checks. Measured payload intake is **3.23 Mb/s** at
5 Msym/s with the PSRAM encoder (radio off, 2026-10-17), suitable for files,
telemetry and low-rate video.

Its console protocol:

- `spiinput usb [ts|gcs]` selects USB for the next run. Wait for `usbinput ready:true`, then
  send messages. USB pacing replaces READY, and messages may arrive split anywhere.
- A NOP message means **end of stream** on USB. On `usbinput stop:true`, finish the current
  message, send a NOP, and send nothing more until the command's `done:true` record.
- Before each RX read, firmware masks the driver RX interrupt and checks its software queue
  first, then polls the FIFO one 64-byte packet at a time, acknowledging before reading.
  The handoff excludes the USB ISR so the two readers cannot reorder bytes. Re-checking the
  driver is essential: console TX can restore an older interrupt-enable value after a pause.
  Console TX remains with the driver; the prior RX-enable bit is restored after the session.
  When the encoder pauses, the endpoint applies USB backpressure.
- The opening station ID also pauses the encoder once its output ring fills.
  Write timeouts must cover the ID (5.22 seconds for N0CALL), including writes of
  the final NOP. Radio-off pace adds ID time to the requested payload duration;
  the keyed run retains its wall-time limit. RX runs in the encoder task itself,
  with notification sleeps for input and output waits. `usb.trace[i]` records
  read calls and payload bytes in each second after ready (first 64 seconds), so
  the result exposes whether reception resumes after ID. `last_message_us` uses
  the same origin; `us`/`mbps` retain the first-to-last-message wall measurement.
- After a run, a missing NOP prints an error and restarts the S3 within three seconds, with
  RF already off. Partial binary data is never passed to the console parser. The sender keeps
  reading after write failures to save the board counters and restart status in its report,
  then its next `info` probe handles USB re-enumeration. Uncertain writes are never retried.
