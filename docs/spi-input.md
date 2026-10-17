# Payload over SPI

The first board run passed payload correctness but failed paced input and Wi-Fi
allocation. [Unchanged coordinator records](../results/2026-10-17-spi-in-first/README.md)
contain the measurements. The revised code below is host-tested; its hardware
throughput, encoder deadlines and 20,000 B Wi-Fi reserve still need board checks.

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
| GND | GND | GND | pin 6 |

The CM5 table assumes the standard IO-board header, not bare module connector
numbers. [Raspberry Pi SPI pin mapping and enablement](https://www.raspberrypi.com/documentation/computers/raspberry-pi.html#serial-peripheral-interface-spi);
[XIAO pinout](https://wiki.seeedstudio.com/xiao-esp32s3-freertos/).
`spipins xiao`, `spipins devkit`, or `spipins CLK CS D0 D1 D2 D3 READY` overrides
pins until reset. All seven S3 GPIOs must be distinct and available.

Two S3s use the same row-to-row wiring, all four data lines, slave READY to master
READY. A single bare board instead reads SPI3 outputs through SPI2 on the same pads.
**Disconnect all external devices and jumpers for `loop` tests.**

## PV-SPI v1

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

## Monday: CM5, 8PSK 2/3 at 10 Msym/s

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

The independently written [sender](../host/cm5_spi.py) uses mode 0, 8 bits/word,
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

## Separate sender, receive cost and memory

The first loopback's priority-2 sender shared core 1 with the priority-23 timed
worker. Only 39 messages arrived during 40.44 s of paced scheduling, while an
untimed USB dump decoded exactly. That loopback was not an external-source model.
Now encoder loopback sends on **core 0, priority 6**, above the priority-5 encoder,
and sleeps on a microsecond timer between messages. Standalone `spirx` uses core 1,
priority 2. `spiinput ts loop 22` gives the demo a fixed offered-rate ceiling. Sender
preemption is included in encoder wall-cycle windows. High rates can still overload
core 0. Never interpret its achieved rate as an external RX limit. At 40 MHz, one
quad transfer takes 67.2 µs, so polling alone occupies at least 14.0% of a core
at 22 Mb/s offered, or 35.1% at 55 Mb/s. Using the measured synthetic encoder's
`227692/299538=76.0%` as a proxy gives **111.1%** total at 55 Mb/s before receive
CRC/copy and sender CRC costs. This is an overload estimate: input replaces synthetic
generation, and READY can reduce the actual sender rate. The external sender has
none of this polling cost on the receiving S3.

`sender.cpu_us / sender.us` is the measured scheduled CPU fraction on its reported
core (FreeRTOS ESP-timer runtime statistics; ISR time can be attributed to the
interrupted task). Build, polling/build and diagnostic CRC windows are cycle-counter
wall intervals and can overlap/include preemption. They must not be added as CPU
usage. `sender.heap_bytes` excludes the separately reported 2,048 B task stack/TCB.
The coordinator gate requires at least 256 B of sender stack unused at its
low-water mark (`stack_min_free`); the smaller stack still needs that board check.
Its second GDMA channel reads every message. The external sender adds **zero** S3
sender task, SPI3 driver allocation or transmit DMA traffic.

Per full message, minimal receive SRAM traffic is `1332 DMA write + 1328 CRC read +
2*1316 copy = 5292 B`. The loop sender adds `1332 construction writes + 1328 seal CRC
reads + 1316 diagnostic CRC reads + 1332 DMA reads = 5308 B`. Both exclude table
lookups, descriptors, instruction traffic and the encoder. At demo rate, 1838.58
messages/s imply **9.73 MB/s receive + 9.76 MB/s sender**; at 46.56 Mb/s, about
**23.40 + 23.47 MB/s**. These are derived lower bounds, not measured bus occupancy.
At 40 MHz quad SPI, modeled payload capacity is 136.37 Mb/s including the 10 µs gap;
measured one-board pattern verification only achieved 43.17/64.77 Mb/s.

`spiinput ts replay` or `gcs replay` preloads four sealed pattern messages and
repeats them through the real parser, CRC, split copies, descriptor release and
encoder. Sequence tracking resets on replay release; repeated sequences are a
diagnostic fixture, never a wire protocol change. It allocates SPI2/receive DMA
resources but leaves DMA idle, with READY low, no SPI3 or sender task. `--replay`
tells the host decoder to expect pattern sequences 0..3 repeated. This isolates
**receive computation**, not external DMA bandwidth, interrupt latency or cable
integrity. Those require an external sender; replay cannot certify them.

`input_cycles` includes CRC, copy, parsing and descriptor release; sleeps awaiting
bytes are excluded. `input_copy_cycles` is the memcpy subset, already included.
Divide by `frames_encoded` for cycles/frame (a final unfinished frame can also
contribute). `input_frame_cycles_max` is active RX work for one completed frame;
`encode_cycles_live_max` includes that RX work plus encoding. ISR callback totals
and maxima exclude interrupt entry/exit; some callback windows already overlap
active measurements. Do not double count them. External paced zero-underrun tests
are the deadline proof.

For QPSK 9/10 at 80/3 Msym/s: `B=7264 B`, `S=33282 symbols`,
`budget=240e6*S/(80e6/3)=299538 cycles/frame`, `R=46.56 Mb/s`.
The [synthetic mean](../results/2026-10-17-qpsk-goal/README.md) is 227692 cycles,
leaving 71846 cycles. The old GCS dump's live max 289707 left only 9831 cycles;
it was not a paced pass. Its measured 1,316 B copy maximum of 2301 cycles projects
`7264/1316*2301=12701 cycles/frame`, **an estimate**, not a maximum for a full frame.

Memory changes, derived from the layout:

- Four slots need **5,344 B**, half the old 10,688 B. They borrow whole disjoint
  slots from unused shaper-store/workspace tails when all four fit. At QPSK n3,
  two fit after the packed dummy and two after the shaper window in the workspace:
  **zero additional receive-buffer heap**. S3 descriptors need 48 B.
- Packed external payload borrows fast encoder scratch before encoding. No second
  payload allocation is needed. Input uses nibble BCH, saving the 8,192 B byte
  cache in modes that used it; bit-exact host checks cover both BCH variants.
- At 8-sps 8PSK the external-input fast context moves to PSRAM, as its workspace
  already does. This preserves SRAM but changes cache contention. Its deadlines
  must be checked separately; the 10 Msym/s demo uses internal context/workspace.
- The diagnostic SPI master lives in flash. Its old linked IRAM contribution was
  3,937 B; the new map has zero. Receive callbacks and hot CRC/copy remain in IRAM.

Wi-Fi must start with receive buffers still allocated and retain **at least
20,000 B** free and low-water internal SRAM, for TS and GCS in every selected mode.
The main-branch 41,387 B was its low-water mark; free-up was 43,203 B. These layout
savings predict recovery but do not establish a new free-up measurement.

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
replay at 26m667, receive-only Wi-Fi probes, isolated throughput/faults, then synthetic
CRCs. It continues after failures, writes `summary.txt`, exits nonzero if any gate
fails, and restores input off. Default paced windows are 10 s; set
`SECONDS_PER_RUN=30` for acceptance. Set `WIFI_ALL=1` for receive-only Wi-Fi
probes in every supported MODCOD/rate (TS and GCS), rather than the six default
representatives. `PY`, `RX`, `PINS`, `PORT` override local paths.

Pass requirements: exact dump bytes and USB/file CRC; independent LeanDVB decode
with no FEC corrections/failures; zero normal paced underruns, late fills/switches
or fatal lateness. Host captures acquire QPSK within two frames and 8PSK within four;
the 40-frame demo must recover at least 36 contiguous exact frames. At 26m667,
fill <=40137 cycles, slack >=7083, encoder mean <=254607 and live max <=299538,
including active input copies. Deliberate fault-6 starvation must insert dummies
and recover. Synthetic CRCs remain 1271511239 (11/26m667/16 frames) and 3178964763
(13/20ms6/40 frames), with N0CALL.

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
