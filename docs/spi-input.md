# Payload over SPI

`spiinput pv` accepts PigeonVision's **raw PV-SPI v1** TS messages: mode 0,
MSB first, single-line, up to **20 MHz**, exactly **1,332 bytes per CS assertion**.
There is **no command, address or dummy-clock prefix**. Native `pv-capture`
works directly with its existing SPI configuration and starts sequence zero.
Select a fresh S3 receiver run before starting each sender. `spiinput off`
restores synthetic input. These commands only select input for the next run;
they do not start the receiver or enable RF.

This revision is host-tested and builds with ESP-IDF v6.1. No board, serial port
or radio was accessed during development. External 20 MHz reception and sustained
9 Mb/s camera input still need the coordinator's hardware check.

## Coordinator radio-off check, about five minutes

**Pass criteria first:** clean loopback has nonzero accepted TS, correct pattern,
matching message CRC chain and zero short/long/header/CRC/sync/sequence/overrun
errors. Deliberate faults must increment their respective counters and recover.
The PV dump must decode to the offered TS; synthetic dump CRCs must remain
**QPSK 1271511239** and **8PSK 3178964763**. Paced output must have zero late bank
switches/fills and no encoder failure. At **9 Mb/s**, dummy frames are expected
because MODCOD 13 at 10 Msym/s consumes **19.356583 Mb/s** TS. Dummies do not mean
SPI loss. Missing tools, failed commands and failed gates must produce FAIL,
while later steps still run. Inspect every PASS/FAIL line and retain the logs.

On an already flashed, **bare board**, disconnect the CM5 and every SPI jumper.
The coordinator owns the only board session. Set `PORT`, `PINS=devkit` or `xiao`,
and `RX` to the LeanDVB executable as needed, then run:

```sh
PINS=devkit bash host/pv-spi-board.sh
```

The script keeps going, stores evidence under `notes/`, and restores input off
on exit. It runs a 20 MHz raw one-board sender at 9 Mb/s, CRC/short/long faults,
a 5 Msym/s PV dump at a 12 Mb/s offered ceiling, a 30 s 9 Mb/s paced run, the
kept quad mode, and both synthetic CRC regressions. SPI3 drives SPI2's inputs
through the same pads; no loopback wires are needed. It cannot establish CM5
scheduling margin, cable timing or RF performance. An individual check is
`spirx 3000 pv loop 9`; encoding uses `spiinput pv loop 9` before `dvbpace`.
`spisend ms pv [MHz [Mb/s [fault]]]` also supports a second S3, capped at 20 MHz.

The dump uses **40 MS/s / 5 Msym/s = 8 samples/symbol**, without decimation.
The decoder must use that ratio; 4 samples/symbol produces false MODCODs even
when LeanDVB reports frame lock. The dump runs at a **12 Mb/s** offered ceiling,
above its **9.678291 Mb/s** TS demand (5,370 bytes/frame × 8 bits/byte ×
5e6 symbols/s / 22,194 symbols/frame).
The separate paced check uses 9 Mb/s and may insert dummies.

Firmware gates use ESP-IDF's `git describe --always --tags --dirty`, compared
exactly after its 31-character application-version limit. Failure output shows
the full expected version and the board's field. Long tags can truncate the hash
or dirty suffix; this field cannot distinguish versions beyond that limit.

## CM5 with unchanged pv-capture

Enable SPI0 (`dtparam=spi=on`). Keep the existing camera settings, including:

```json
{"mux_bitrate":9000000,"udp_destination":null,
 "spi":{"device":"/dev/spidev0.0","hz":20000000,"ready_line":25}}
```

`spi.gpiochip` can identify the RP1 chip explicitly; otherwise native pv-capture
finds it. Start an S3 radio-off receiver run such as `spiinput pv`, then
`dvbpace 13 10m 120`, before starting capture. Stop capture several seconds before
the S3 run ends, and let accepted input drain. Do not change the capture transport
or add the old S3 prefix. Camera capture and recording remain owned by PigeonVision.

Compare final `outputs.transport.spi` with S3 `spi`: **messages, ts_packets,
payload_bytes, crc_chain, next_seq** must agree after draining, with zero receiver
protocol errors and no capture transport drops/timeouts/uncertain writes.
`crc_chain = zlib.crc32(message[1328:1332], previous_chain)`, initially zero,
for accepted TS_DATA only. NOP advances sequence but does not enter these counts.
`msgs`/`bytes` remain aliases of accepted data-message/payload counts. Receiver
counts mean parsed acceptance, including any partial encoder frame pending at
stop; sender counts mean completed writes, with no acknowledgment. At run expiry,
unparsed/prefetched bytes can be discarded. Sequence gaps count skipped messages;
duplicates/backward messages are rejected. Start a new receiver run when restarting
capture. The counters are 32-bit; compare runs below 4 GiB of payload.

The S3 keeps TS bytes in order across message and DATA FIELD boundaries. Its
existing TS adapter replaces sync bytes with packet CRC-8 during DVB-S2 encoding.
When input is empty, the timed output emits **3,330-symbol DVB-S2 dummy frames**;
it preserves partial TS state until more data arrives. Unlike the RP2350 receiver,
it does not synthesize PID 0x1fff TS null packets. No dummy or null byte enters the
SPI acceptance counts or CRC chain. TS continuity counters and PCR remain the mux's job.

## Raw PV-SPI contract

The payload layout is the table in the kept-format section below, restricted to
NOP (type 0, length 0) and TS_DATA (type 1, 1..7 complete 188-byte TS packets).
All multibyte fields are little endian, reserved fields zero, and unused payload
bytes are zero padded by the sender. CRC-32 covers bytes 0..1327, reflected IEEE
polynomial 0xedb88320, init/final XOR 0xffffffff. The check value for `123456789`
is `0xcbf43926`; the one-packet sequence-1 vector seals to **0x51be328d**.
`examples/pi-sender/send.py --vector` verifies that vector without hardware.

READY is active high only after DMA is armed with room for another message.
After CS rises, wait **at least 10 µs**, then read READY immediately before each
transfer. Send only while high; finish a started message even if READY falls.
Do not queue from an earlier READY sample. The raw slave lowers READY in its
completion callback and raises it after the next DMA setup, subject to the
existing two-slot credit margin. Four internal 1,336-byte buffers hold one CS
transaction each. A short/long or bad message is discarded as a unit; the next
CS starts fresh. Clocks sent while no transaction is armed are outside the
contract and may not be counted. MISO is unused; v1 has no ACK or retry.

Wire budget at 20 MHz: `T = 1332*8/20e6 + 10e-6 = 542.8 µs/message`.
The ideal payload ceiling is `1316*8/T = 19.396 Mb/s`, before GPIO, ioctl and
scheduling costs. A 9 Mb/s stream needs `9e6/(1316*8) = 854.86 messages/s`,
so ideal bus occupancy is **46.4%**. This calculation is not measured throughput.
Check SCK, mode, CS lengths/gaps and READY on the final wiring at 1 MHz, then 20 MHz.

Read-only contract sources: [PigeonVision flight SPI notes](https://github.com/stanford-ssi/PigeonVision/blob/main/software/flight/spi.md),
its `software/python/pigeonvision/spi_transport.py` and native
`software/flight/include/pv/spi_protocol.hpp` / `src/spi_output.cpp`, plus the
[RP2350 specification](https://github.com/Rivercraft911/RP2350_IQ_Benchmark/blob/main/docs/pv-spi-spec.md),
`firmware/src/ingress/pvspi.c`, `pvproto.c` and
[CM5 results](https://github.com/Rivercraft911/RP2350_IQ_Benchmark/tree/main/results/cm5-spi).
The raw path uses the [ESP-IDF SPI slave driver](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32s3/api-reference/peripherals/spi_slave.html),
with word-aligned DMA buffers and setup/completion callbacks. The quad path below
retains its original GDMA descriptor chain and command phases.

## Earlier quad measurements

The [clean third run](../results/2026-10-17-spi-in-third/README.md), firmware 39d9c04,
passed replay and standalone 46.6 Mb/s, but the demo loopback still inserted
5,839 dummy frames in 30 s. Its sender preempted the encoder on core 0, and input
also selected slower BCH. Round four removes those differences and stages TS
CRC tables in existing encoder scratch. These changes need a fresh board run.
No external-source rate is qualified yet.

`spiinput ts` or `spiinput gcs` selects an external sender for the next `dvbdump`,
`dvbpace`, `wifimem` or `dvb`. `spiinput off` restores synthetic payload. The sender
is normally a CM5 or second S3. These input commands do not transmit RF.
TS sync replacement and packet CRC-8 follow EN 302 307-1 §§5.1.4–5.1.6; GCS bytes
pass unchanged. No station/application header is inserted into either input.

## Wiring

3.3 V logic, common ground, short leads. READY is an S3 output, active high.
Do not join separately powered boards' 3V3 outputs. Clock/pin timing needs a scope
or logic-analyser check on the actual cable.

| Signal | DevKit GPIO | XIAO GPIO / header | CM5 IO-board 40-pin header |
|---|---:|---|---|
| CLK | 12 | 7 / D8 | GPIO11, pin 23 |
| CS, active low | 10 | 1 / D0 | GPIO8 / CE0, pin 24 |
| D0 / MOSI | 11 | 2 / D1 | GPIO10, pin 19 |
| D1 | 13 | 4 / D3 | unconnected for single-line SPI |
| D2 | 14 | 5 / D4 | unconnected |
| D3 | 9 | 6 / D5 | unconnected |
| READY | 8 | 8 / D9 | GPIO25 input, pin 22 |
| GND | GND | GND | pin 20 or 25 (pin 6 also GND) |

For raw PV-SPI, connect only CLK, CS, D0/MOSI, READY and GND. Leave
MISO and D1/D2/D3 disconnected. Fit a 4.7 kΩ pull-up from CS to **S3 3.3 V**
and a 4.7–6.8 kΩ pull-down from READY to GND. Power the S3 before the CM5 drives
SPI. Both boards use **3.3 V signals**; no 5 V GPIO or power-header connection.
Use more than one ground lead if available, with ground alongside CLK.
DevKit defaults are GPIO12/10/11/8; XIAO defaults are GPIO7/1/2/8.
The unused quad pins are neither configured nor driven in raw mode.

The CM5 table assumes the standard IO-board header, not bare module connector
numbers. [Raspberry Pi SPI pin mapping and enablement](https://www.raspberrypi.com/documentation/computers/raspberry-pi.html#serial-peripheral-interface-spi);
[XIAO pinout](https://wiki.seeedstudio.com/xiao-esp32s3-freertos/).
`spipins xiao`, `spipins devkit`, or `spipins CLK CS D0 D1 D2 D3 READY` overrides
pins until reset. All seven S3 GPIOs must be distinct and available.

Two S3s use the same row-to-row wiring, all four data lines, slave READY to master
READY. A single bare board instead reads SPI3 outputs through SPI2 on the same pads.
**Disconnect all external devices and jumpers for `loop` tests.**

## Prefixed S3 format (kept)

`spiinput ts` and `spiinput gcs` keep the original prefixed format.
This is distinct from raw PV-SPI v1 selected by `spiinput pv`.
One message per CS assertion, mode 0, MSB first. Command **0x03** selects single-line
CM5 data; **0x23** selects four-line S3 data. Command and address are always on D0:
command byte, address **0x00**, **eight dummy clocks**, then 1,332 data bytes.
In quad mode send the high nibble first, bit 3 on D3. Do not set multiline-address
flags. CS setup and hold must each be at least two SCK periods.

After CS rises wait **at least 10 µs**, then sample READY immediately before the
next CS assertion. Send only if high. Finish a started message even if READY falls.
Never queue messages from an old READY sample. A low READY pauses the producer;
there is no acknowledgement or retransmission.

S3 [TRM v1.8](https://documentation.espressif.com/esp32-s3_technical_reference_manual_en.pdf)
§30.5.9.2, pp.1136–1138, Table 30.5-14 specifies these commands and phase lengths.
CS rising ends receive in single-transfer mode (§30.5.6.1 p.1118, Table 30.5-7;
§30.5.9.4 pp.1139–1140). The older serial-link page's four dummy clocks do not apply.

| Offset, bytes | Meaning |
|---|---|
| 0..1 | ASCII `PV` |
| 2 | version 1 |
| 3 | 0 NOP, 1 TS, 2 GCS |
| 4..5 | sequence, little endian, increments per message including NOP; wraps at 65536 |
| 6..7 | payload length, little endian |
| 8..11 | zero |
| 12..1327 | payload then zero padding |
| 1328..1331 | little-endian IEEE CRC-32 of bytes 0..1327, equal to `zlib.crc32` |

NOP length is zero; GCS length is 1..1316 B. TS contains 1..7 complete 188 B packets,
each starting 0x47. The session type is fixed. Header, length, CRC and TS sync errors
discard a whole message. Duplicate/backward sequences are discarded; forward gaps
are counted. Fewer than 32768 missing messages can be distinguished from backwards
sequence. Restart the receiver run when restarting a sender's sequence.

Four 1,336 B internal DMA slots each leave four bytes beyond an exact message.
Two armed slots are required for READY high. The encoder drains partial frames
across messages, so the receive ring need not hold a whole DATA FIELD. Exhaustion
holds READY low until data drains; overrun recovery resets DMA with CS high.
Ignoring READY can lose data. Descriptor ownership/chaining: TRM §3.4.1 pp.358–359,
§§3.4.5–3.4.7 pp.362–364.

The encoder and receive DMA interrupt run on core 0. The timed shaper occupies core 1.
It never waits for SPI: complete 3,330-symbol dummy frames cover starvation, then
data resumes in order. TS packet state survives dummies. There is no finite-stream
flush; stopping discards partial/prefetched data.

## Legacy prefixed sender: 8PSK 2/3 at 10 Msym/s

Use MODCOD 13 and rate token `10m` (40 MS/s, four samples/symbol), external TS.
Payload `B=(43040-80)/8=5370 B`; pilots give `S=21600+90+14*36=22194 symbols`.
At 240 MHz, `Tframe=S/10e6=2.2194 ms`, `budget=532656 cycles/frame`,
`R=8*B*10e6/S=19.356583 Mb/s`. Inputs: encoder mode table; EN 302 307-1
Tables 5a/12 and §§5.5.3–5.5.4. Video/audio must fit below this **total TS** rate.
Use TS null padding to fill it. An unpadded 10 Mb/s producer deliberately causes
dummy frames; it cannot supply a continuous 19.36 Mb/s DATA FIELD stream.

Enable `dtparam=spi=on` in CM5 `/boot/firmware/config.txt` and reboot. Confirm
`/dev/spidev0.0`. Install Python `spidev` and **libgpiod v2** bindings (`gpiod>=2,<3`)
in a virtual environment if the OS packages provide v1. `gpiodetect`/`gpioinfo`
identify the RP1 GPIO chip and line 25; some images number it `gpiochip4` rather
than `gpiochip0`. Give the sender account access to that chip and SPI device.
[Linux spidev API](https://docs.kernel.org/spi/spidev.html),
[Python SPI API](https://github.com/doceme/py-spidev),
[libgpiod bindings](https://git.kernel.org/pub/scm/libs/libgpiod/libgpiod.git/tree/bindings/python).

The legacy prefixed [sender](../host/cm5_spi.py) uses mode 0, 8 bits/word,
25 MHz requested SCK, one synchronous `xfer2` of **1,335 bytes** (three prefix bytes
plus PV message), and a fresh READY read after each 10 µs gap. It never queues SPI.
It validates TS sync, pads a final 1..7-packet batch and calculates each CRC.
For an already padded TS file:

```sh
python3 host/cm5_spi.py demo.ts --gpiochip /dev/gpiochip0
```

For continuous paced input, an example producer is:

```sh
ffmpeg -re -stream_loop -1 -i demo.ts -c copy -muxrate 19356583 -f mpegts pipe:1 |
  python3 host/cm5_spi.py - --gpiochip /dev/gpiochip0
```

Start the CM5 sender before the receiver; it waits for READY. Coordinator selects
`spiinput ts`, then first runs `dvbdump 13 10m 40 128 1` and `dvbpace 13 10m 30`.
Use a pattern sender on a second S3 for automated expected-byte dump checks; real
MPEG-TS needs comparison to a saved copy of the offered stream. RF operation still
requires the separate transmission authorization and attenuated/dummy-load setup.

Single-line capacity model at 25 MHz:
`Tmessage=(8+8+8+1332*8)/25e6+10e-6=437.2 µs`,
`Rmax=1316*8/Tmessage=24.08 Mb/s`, before GPIO/ioctl/scheduling overhead.
The demo requires one full message every 543.90 µs, leaving **106.70 µs/message**
for overhead beyond that model. Check actual SCK, CS setup/hold, gap, READY, zero
transport errors and zero underruns with the CM5. This is a margin calculation,
not measured Linux throughput. Ordinary single-line 25 MHz SPI cannot supply the
46.56 Mb/s high-rate QPSK case. Use the second S3's quad sender for that check.

## Pacing and what loopback proves

The second run's demo had zero transport errors, 10,009 encoded frames and
23,399 dummy frames in 30 s of payload scheduling. Its active encoder mean/live
maximum were 375,739/433,311 cycles, below the 532,656-cycle frame budget.
The sender saw READY low 9,994 times. Both sides added avoidable waits: the
encoder waited after successful partial reads, and the sender used one-tick
sleeps after READY low. One full 1 ms tick consumes 45.1% of a demo frame period.
The encoder now drains partial reads immediately and waits only after zero bytes;
the sender polls READY with a 50 µs timer sleep. Host tests enforce both changes.
Only a fresh paced board run can establish zero underruns.

Loopback encoding now uses a core-0 priority-2 sender below the priority-5 encoder.
It runs while the encoder waits for input or a free frame slot. Standalone
`spirx` uses core 1, priority 2. `spiinput ts loop 22` sets an offered
ceiling, not a guaranteed delivered rate. Sender CPU, transmit DMA and pattern/CRC
construction all consume resources absent with an external sender. At 40 MHz,
one quad transaction takes `(24+1332*2+4)/40e6 = 67.3 µs`, including four configured
CS setup/hold clocks. With the mandatory 10 µs gap the wire-only ceiling is
`1316*8/77.3e-6 = 136.20 Mb/s`. GPIO, driver, timer and verification costs reduce it.

Measured standalone verified rates were **22.020, 46.768 and 57.650 Mb/s** at
22, 46.6 and 87 Mb/s ceilings. Thus the one-board fixture proves error-free receive
plus pattern checking at 46.768 Mb/s for that 1 s run; the saturated run reached
57.650 Mb/s, not 87. It proves neither a receiving S3's external-source ceiling nor
paced encoding at those rates. The script gates 22/46.6, then reports untargeted
`loop 0` saturation separately. A second S3 is required to test external quad DMA;
the CM5 is required to qualify its single-line Linux sender.

`sender.cpu_us / sender.us` is measured scheduled CPU fraction (ISR time may be
charged to the interrupted task). Build, polling/build and CRC cycle windows
include preemption and overlap; do not add them as CPU usage. `heap_bytes` excludes
the 2,048 B sender stack/TCB. Its measured low-water stack margin must stay >=256 B.

Per full message the receive traffic lower bound is `1332 DMA write + 1328 CRC read
+ 2*1316 copy = 5292 B`. Loopback adds `1332 construction + 1328 seal CRC + 1316
verification CRC + 1332 DMA read = 5308 B`, excluding tables/instructions/descriptors.
At the demo's 1838.58 messages/s these are 9.73 and 9.76 MB/s respectively, not
measured bus occupancy.

`spiinput ts replay` or `gcs replay` repeats four sealed pattern messages through
the actual parser, CRC, split copies, descriptor releases and encoder. DMA remains
idle and READY low. Sequence tracking resets on replay release, a diagnostic fixture
only. `--replay` selects the corresponding host oracle. The second run's QPSK TS/GCS
replay passed 30 s with zero underruns, mean 245,089/208,957 cycles and live maximum
260,542/222,688 cycles. Replay isolates receive computation; it cannot certify DMA
contention, interrupt latency or cable timing. Demo replay precedes high-rate replay
in the revised script to distinguish receiver cost from loopback interference.

`input_cycles` includes active parsing, CRC, copies and release, excluding sleeps.
`input_copy_cycles` is already included. Divide by `frames_encoded` for cycles/frame;
a final partial frame may also contribute. `encode_cycles_live_max` includes active
RX and encoding, plus preemption, but excludes input waits. ISR callback windows
exclude entry/exit and can overlap active windows. `isr_max` now copies the recorded
maximum into telemetry; the old zero was a reporting omission.

The failed 257,061-cycle QPSK mean was **synthetic input**, after `spiinput off`.
Its payload stage averaged 88,080 cycles versus 58,763 in the earlier QPSK result;
the other stages were nearly unchanged. The compiled scalar generator used a taken
branch per byte. Four-byte grouping now produces the identical xorshift32 sequence
with a hardware loop. This is an instruction-count improvement awaiting measurement;
the 254,607-cycle mean and 299,538-cycle live-maximum gates remain unchanged.
For QPSK 9/10, `B=7264 B`, `S=33282 symbols`, `R=8*B*(80e6/3)/S=46.56 Mb/s` and
`240e6*S/(80e6/3)=299538 cycles/frame` (mode table and EN 302 307-1 §5.5).

## Internal RAM after merging main

Requested sizes come from `start_encoder` and `dvbs2_fast_mode_size`, in bytes.
The fixed shaper store is 65,536 B; two separate DAC banks reserve 131,072 B.
Input borrows context scratch for payload, with no separate packed-input payload.
The demo and 8PSK 20m now also borrow 8,192 B in the store for byte BCH. Other
input modes retain nibble BCH. Target context size is
`F=592+max(96*(nbch/360)+1536*ceil(q/32), bits==3 ? 10816 : 16224) B`;
`nbch` and `q` come from the mode table, and 592 B is the aligned S3 context header.

| Mode | Table, B | Ring/dummy in store, B | Workspace heap, B | Other internal heap, B | RX slots |
|---|---:|---:|---:|---:|---|
| QPSK default | 32,768 | PSRAM | 12,420 PSRAM | F + 420 window | 4 in store |
| QPSK 5m | 8,192 | 33,344 + 1,696 | 12,420 | F | 4 in store |
| QPSK 10m | 4,096 | 50,016 + 1,696 | 12,420 | F | 4 in store |
| QPSK 20m | 49,152 | 864 dummy + 12,432 workspace | 0 | F + 25,056 ring | 2 store + 2 workspace |
| QPSK 26m667 | 36,864 | 25,056 + 864 | 12,420 | F | 2 store + 2 workspace |
| 8PSK 5m | 65,536 | none | 12,420 PSRAM | 23,936 ring/dummy + 2,058 window + 5,344 RX | heap |
| 8PSK 10m, MODCOD 13 | 32,768 | 22,240 + 1,696 + 8,192 BCH | 12,420 | F + 11,120 ring + 5,344 RX | heap |
| 8PSK 10m, other MODCODs | 32,768 | 22,240 + 1,696 | 12,420 | F + 11,120 ring | 4 in store |
| 8PSK 20m, t > 8 | 16,384 | 33,360 + 1,696 + 8,192 BCH | 12,420 | F | 4 in store |

8PSK 5m also puts F in PSRAM. Packed frames include 16 readable guard bytes and
16-byte rounding. Four receive slots occupy `4*1336=5344 B`; descriptor storage
is 48 B static. At QPSK 20m, the store tail has 3,088 B and the workspace tail
has 3,236 B, each fitting two slots. At 26m667 the respective tails are 2,752 and
2,900 B. All inputs and outputs stay disjoint. Task/driver/allocator overhead is
excluded from requested sizes and included in board heap telemetry.

Main reclaimed 4,096 B of static internal RAM
([cleanup record](../results/2026-10-17-cleanup/README.md)). Applying only that
saving to each second-run minimum of Wi-Fi-up free/low-water heap, across TS/GCS,
gives the estimates below. New `20m` uses old `20mq4` for QPSK and `20ms6` for 8PSK;
its heap layout is compared to that exact old kernel. Values are **predictions,
not measurements of this revision**. The odd first QPSK-5m minimum is retained.

| MODCOD | F, B | Default, B | 5m, B | 10m, B | 20m, B | 26m667, B |
|---|---:|---:|---:|---:|---:|---:|
| 4 | 16,816 | 46,683 | 32,787 | 34,695 | 22,059 | 34,695 |
| 5 | 16,816 | 46,683 | 34,695 | 34,695 | 22,059 | 34,695 |
| 6 | 16,816 | 46,683 | 34,695 | 34,695 | 22,059 | 34,695 |
| 7 | 16,816 | 46,683 | 34,695 | 34,695 | 22,059 | 34,695 |
| 8 | 17,488 | 46,011 | 34,023 | 34,023 | 21,387 | 34,023 |
| 9 | 16,816 | 46,683 | 34,695 | 34,695 | 22,059 | 34,695 |
| 10 | 17,488 | 46,011 | 34,023 | 34,023 | 21,387 | 34,023 |
| 11 | 17,680 | 45,819 | 33,831 | 33,831 | 21,195 | 33,831 |
| 12 | 15,568 | n/a | 32,543 | 24,819 | 35,943 | n/a |
| 13 | 15,184 | n/a | 32,543 | 25,203 | 36,327 | n/a |
| 14 | 16,624 | n/a | 32,543 | 23,763 | 34,887 | n/a |
| 15 | 16,528 | n/a | 32,543 | 23,859 | 34,983 | n/a |
| 16 | 17,488 | n/a | 32,543 | 22,899 | 34,023 | n/a |
| 17 | 17,680 | n/a | 32,543 | 22,707 | 33,831 | n/a |

Those estimates describe the preceding nibble-BCH allocation policy. Round four
reduces the encoder stack from 4,096 to 3,072 B, reclaiming 1,024 B per command,
and reports `encoder_stack_min_free`; the new gate requires at least 256 B unused.
For MODCOD 13/10m, the clean run's 25,131 B Wi-Fi minimum predicts approximately
`25131 + 1024 - 5344 = 20811 B`, before allocator/code-placement changes. Its
new receive slots are on the internal DMA heap because BCH occupies the store
tail. Other 10m modes keep borrowed slots. At 20m the store fits BCH and all four
slots. The all-mode sweep must confirm every minimum remains at least 20,000 B.

## Round four: measured cause and proposed correction

The clean run's TS loopback averaged 454,326 cycles/frame, versus 250,454 for TS
replay and 181,925 for GCS replay. QPSK used replay, so it had no sender task.
The old sender used 32.1% of scheduled core-0 CPU time. It could interrupt every
encoder stage and even the measured input memcpy. The input layout was internal
RAM, with the same addresses in replay. Reads already drained whole fragments;
neither byte-sized copies nor PSRAM input slots explain this comparison.

The new lower-priority sender cannot preempt active encoding. The 50 µs READY
timer remains, with sleeping polls rather than a busy loop. Timer/DMA interrupts
can still preempt. The demo retains byte BCH by borrowing the 8,192 B store tail;
8PSK 20m has room for the same policy. The TS stage copies its 1,024 B CRC-8
tables into idle LDPC parity scratch before use. The borrowed payload precedes
that scratch; LDPC overwrites the tables only after TS/BCH finish. No context,
workspace, DMA slot, shaper table or frame buffer overlaps another live user.

An estimate using the [cleanup baseline](../results/2026-10-17-cleanup/runs.jsonl)
is `167703 - 43528 synthetic payload + about 30000 receive + about 23000..27000
TS adaptation = about 177000..181000 cycles/frame`, before additional interrupt
cost. The TS estimate starts from 17 scalar instructions per four-byte CRC step
in `dvbs2_pie_crc8`, plus packet edges/calls; it is not a measured cycle count.
The prediction is close to the requested limit and must be measured.

The script now records a fresh no-input demo pace, both TS/GCS loopback paces and
both replay paces, with equal durations. For each input run it enforces
`mean <= 1.05 * no_input_mean + input_copy_cycles / frames_encoded`.
This includes receive parsing/CRC, TS adaptation and preemption. Copy is an
allowance only once; it remains included in the measured input mean. Starvation
is excluded from this comparison, but still has its original frame-deadline gate.

The alleged GCS MODCOD-5/26m667 heap failure is a timeout in the supplied clean
log, with no result or below-gate value. The TS counterpart had 34,635 B free.
Its cause remains unknown. The board reader now retains partial records and the
last 4 KiB of device diagnostics in timeout errors; it does not retry or reset.

## Coordinator checks, no RF

Build only: `cd firmware; . ~/.espressif/tools/activate_idf_v6.1.sh; idf.py build`.
For an existing sdkconfig, also apply the defaults changes: SPI_MASTER_ISR_IN_IRAM=n,
FREERTOS_GENERATE_RUN_TIME_STATS=y, FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER=y.
The coordinator alone flashes/opens serial. For XIAO's 8 MB flash use:

```sh
idf.py -B build-xiao -D SDKCONFIG=sdkconfig.xiao.local \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.xiao' build
```

`bash host/spi-in-board.sh [new-directory]` runs demo loopback first, receive-compute
replay at 10m and 26m667, receive-only Wi-Fi probes, isolated throughput/faults, then synthetic
CRCs and the fresh no-input demo baseline. It continues after failures, writes `summary.txt`, exits nonzero if any gate
fails, and restores input off. Default paced windows are 10 s; set
`SECONDS_PER_RUN=30` for acceptance. Set `WIFI_ALL=1` for receive-only Wi-Fi
probes in every supported MODCOD/rate (TS and GCS), rather than the seven default
representatives. `PY`, `RX`, `PINS`, `PORT` override local paths.

Pass requirements: exact dump bytes and USB/file CRC; independent LeanDVB decode
with no FEC corrections/failures; zero normal paced underruns, late fills/switches
or fatal lateness. Host captures acquire QPSK within two frames and 8PSK within four;
the 40-frame demo must recover at least 36 contiguous exact frames. At 26m667,
fill <=40137 cycles, slack >=7083, encoder mean <=254607 and live max <=299538,
including active input copies. Deliberate fault-6 starvation must insert dummies
and recover. Synthetic CRCs remain 1271511239 (11/26m667/16 frames) and 3178964763
(13/20m/40 frames), with N0CALL.

The slim Wi-Fi sample covers demo and the main memory layouts. To check another
mode, use `spiinput ts`, `wifimem MODCOD RATE`, then repeat with GCS; do not leave
loopback selected for a receive-only heap measurement. The `WIFI_ALL=1` sweep and
external CM5/S3 paced runs remain required before claiming every mode passes.
On a second S3: `spisend 120000 ts 40 55` (or `gcs`), then receiver `spiinput ts`
and `dvbpace 11 26m667 30`. Use 22 Mb/s for demo, 55 for QPSK headroom. `spisend`
waits for READY. No input command keys RF.

Host checks: `tests/test_ingress.py`, `test_cm5_spi.py`, `test_spi_input.py`,
`test_spi_in_board.py`, `test_spi_sender.py`, `test_dvbs2_c.py`, `test_stream.py`. Stream tests exercise
borrowed receive buffers, partial reads, replay, all allocation-failure cleanup,
starvation/recovery and every DAC word against the plain encoder. Add
`--input-only --receiver /path/to/leandvb` for independent host-capture decoding.
