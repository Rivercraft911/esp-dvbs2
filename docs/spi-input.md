# Payload over quad SPI

`spiinput ts` or `spiinput gcs` selects external payload for the next `dvbdump`,
`dvbpace`, `wifimem` or `dvb` run. `spiinput off` restores the synthetic source.
The radio-off commands never transmit. SPI throughput and timing are **unmeasured**
on this branch. Host tests pass; the coordinator must run the silicon gates below.

SPI2 is a half-duplex slave with GDMA into internal RAM. SPI3 supplies the reference
sender. Both use the same firmware; there is no radio initialization in `spisend`.
A camera computer, flight computer or another MCU can implement the wire protocol.
No application headers are inserted into external data. Existing encoder TS mode
replaces packet sync bytes with CRC-8 and carries packet position across BBFRAMEs;
GCS passes the bytes unchanged (EN 302 307-1 clauses 5.1.4 through 5.1.6).

## Wiring and build

3.3 V logic, common ground. Connect each master signal to the matching slave signal.
READY runs from slave to master. Keep leads short; 40 MHz on these pins is a target,
not a signal-integrity measurement. No pull-up is allowed to substitute for READY.

| Signal | DevKit default GPIO | XIAO default GPIO / header |
|---|---:|---|
| CLK | 12 | 7 / D8 |
| CS, active low | 10 | 1 / D0 |
| D0, first command bit on this line | 11 | 2 / D1 |
| D1 | 13 | 4 / D3 |
| D2 | 14 | 5 / D4 |
| D3 | 9 | 6 / D5 |
| READY, active high | 8 | 8 / D9 |

For two DevKits: **12→12, 10→10, 11→11, 13→13, 14→14, 9→9,
slave 8→master 8, GND→GND**. For two XIAOs use the GPIOs in the last column.
For a mixed pair match rows, not GPIO numbers. Power both boards by their own USB;
do not join their 3V3 outputs. The DevKit assignment avoids flash/PSRAM, USB,
UART, strapping and LED pins. [XIAO pinout and memory](https://wiki.seeedstudio.com/xiao-esp32s3-freertos/).

`spipins xiao`, `spipins devkit`, or `spipins CLK CS D0 D1 D2 D3 READY` overrides
pins until reset. All seven must be distinct output-capable GPIOs. The caller must
choose pins free of board peripherals. Both presets currently use the GPIO matrix.

Normal build: `cd firmware; . ~/.espressif/tools/activate_idf_v6.1.sh >/dev/null 2>&1; idf.py build`.
For the original XIAO's **8 MB flash**, use a fresh build/config:

```sh
idf.py -B build-xiao -D SDKCONFIG=sdkconfig.xiao.local \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.xiao' build
```

`CONFIG_SPI_INPUT_XIAO` selects its pins at boot. Its 8 MB octal PSRAM configuration
is inherited from the normal defaults. Do not flash the DevKit's 16 MB image settings
onto an 8 MB board. The coordinator handles flashing and all serial access.

## Wire protocol, PV-SPI v1

One message per CS assertion, SPI mode 0, MSB first. Send command **0x23** on D0,
address **0x00** on D0, **eight dummy clock cycles**, then 1,332 data bytes over
D[3:0]. Each byte sends its high nibble first, with bit 3 on D3. CS setup and hold
are at least two SCK periods. After CS rises, wait **at least 10 µs**, then sample
READY immediately before the next CS assertion. Send only if high. Finish a started
message even if READY falls. No messages may be queued using an old READY sample.

The S3 hardware defines the command and phase lengths: [TRM v1.8](https://documentation.espressif.com/esp32-s3_technical_reference_manual_en.pdf),
§30.5.9.2, pp.1136–1138, Table 30.5-14. `SPI_TRANS_MODE_QIO` in the IDF sender
selects quad *data*; do not add `SPI_TRANS_MULTILINE_ADDR` for command 0x23.
The TRM specifies eight dummy clocks, despite the older serial-link protocol page's
four-clock table. CS rising supplies receive EOF in single-transfer mode
(§30.5.6.1, p.1118, Table 30.5-7; §30.5.9.4, pp.1139–1140).

| Offset, bytes | Meaning |
|---|---|
| 0..1 | ASCII `PV` |
| 2 | version 1 |
| 3 | 0 NOP, 1 TS, 2 GCS |
| 4..5 | sequence, unsigned little endian, increments per message including NOP, wraps at 65536 |
| 6..7 | payload length, little endian |
| 8..11 | reserved, zero |
| 12..1327 | payload then zero padding |
| 1328..1331 | little-endian IEEE CRC-32 of bytes 0..1327, equal to `zlib.crc32` |

NOP length is zero. TS length is 188*n bytes, n=1..7, every packet starting 0x47.
GCS length is 1..1316 bytes. Session type is fixed by `spiinput`; other types are
counted and discarded. Bad length, header, CRC or TS sync also discards the entire
message. Sequence gaps between accepted messages are counted; duplicate/backward messages are discarded.
There is no acknowledgement/retransmission. GCS loss is visible in counters but
cannot be repaired by this transport. Restart the receiver run after a master
sequence reset; the first valid sequence of a new run establishes its origin.
Fewer than 32768 skipped messages can be distinguished from backward sequence.

Eight 1,336-byte DMA slots leave four bytes beyond a valid message, so an exact
message ends inside one descriptor. Two armed slots are required for READY high.
Ownership is checked by DMA, and the consumer releases slots after copying them.
After exhaustion, READY stays low while accepted data drains; DMA resets only with
CS high. Resume after READY rises. A master ignoring READY loses data. Recovery
from that violation is a board gate, not a delivery guarantee. Descriptor chaining:
TRM §3.4.1, pp.358–359 and §3.4.5–3.4.7, pp.362–364.

The encoder on core 0 owns the receive DMA interrupt. EOF notifies it when bytes
arrive. It keeps a partial payload until a full DATA FIELD is available. The fill
on core 1 never reads SPI or waits for it: the existing ring supplies complete,
3,330-symbol dummy frames during starvation and resumes data in order. TS packet
state survives dummies. There is no timeout padding or finite-file flush. Ending a
run discards partial/prefetched data; start a new stream on the next run.

## Rate and CPU budget: models, not measurements

At 40 MHz: `Tmessage = (8+8+8+1332*2)/40e6 + 10e-6 = 77.2 µs`.
Thus full-message payload capacity is `1316*8/Tmessage = 136.37 Mb/s`, before
sender/interrupt overhead. Short payloads still occupy the whole message.
The targets use 34.2% (46.6 Mb/s) and 63.8% (87 Mb/s) of this modeled link.

QPSK 9/10 with pilots: `B=(58192-80)/8=7264 B`, `S=32400+90+22*36=33282 symbols`.
At 80/3 Msym/s: `240e6*S/(80e6/3)=299538 cycles/frame` and `8*B*(80e6/3)/S=46.56 Mb/s`.
Inputs: encoder mode table and EN 302 307-1 Tables 5a/12, §5.5.3/5.5.4.
[Measured synthetic baseline](../results/2026-10-17-qpsk-goal/README.md): mean
227692 cycles/frame, leaving 71846 cycles. Replacing payload generation changes
that baseline; CRC, copy, interrupts and memory contention must be measured together.

[Measured SRAM memcpy](../results/2026-10-16-memory/README.md), 383.7 MB/s,
predicts `7264*240e6/383.7e6 = 4544 cycles/frame`. A 10k-cycle copy allowance is
3.34% of the current frame period. CRC checking is additional work, not included
in that estimate. The 8PSK moonshot at 100/3 Msym/s would allow 159797 cycles/frame
and carry 87.28 Mb/s; 10k cycles would cost 6.26%. That encoder/fill rate does not
exist here. [53–63 MB/s memory-to-memory GDMA](../results/2026-10-16-dma-offload/README.md)
exceeds the 20 MB/s raw input byte rate numerically, but it is a different transfer
and does not prove SPI throughput or lack of contention. No second DMA copy is used.

Receive buffers use 10688 B internal RAM, descriptors 96 B on S3, CRC tables 4096 B,
plus driver/task state. Seven queued payloads cover 9212 B, more than one maximum
DATA FIELD; back-pressure limits any further backlog. Local loopback adds sender
buffers, driver and stack, so its timing/heap results include work absent with a
separate host. It is useful before the second board arrives, not a substitute for it.

## Coordinator checks, radio off

Bare-board loopback uses SPI3 outputs read back through the same pads by SPI2.
**Connect no jumpers and disconnect external devices from all seven selected pins.**
For actual cable/pin verification use two boards and the wiring above. On the sender
run `spisend 120000 ts 40 55` before starting the receiver (it waits for READY).
On the receiver use `spiinput ts`, then a dump or paced run. Use `gcs` on both sides
for GCS. `spisend ms ts|gcs [MHz [Mb/s [fault]]]` (100..120000 ms) is the reference sender firmware
entry point and builds reproducible test bytes. Replace `ingress_pattern` with a
producer for real data, keeping READY and message construction unchanged.

After flashing, run `bash host/spi-in-board.sh [new-output-directory]` on the
coordinator's machine, with stored N0CALL for the two synthetic regression CRCs.
`PINS=xiao` selects that preset; `PORT`, `PY` and `RX` can override local paths.
The script is about five minutes, fails on a failed gate, saves records/IQ/decodes,
and restores `spiinput off` on exit. It never flashes or keys the radio.

Pass gates fixed before silicon:

- 3 s standalone receives at 46.6 and 87 Mb/s: >=98% of target, zero corruption,
  transport errors/loss/overrun, accumulated received CRC equals the independent
  host pattern. Pattern verification is included in throughput wall time.
- CRC/short/long injections: correct error class, sequence loss, no corrupt accepted
  bytes. Ignoring READY with a 5 ms consumer hold must overrun and recover;
  obeying READY under the same hold must have zero loss and counted back-pressure.
- TS and GCS dumps: 16 frames, zero underruns/overlap errors; USB/file CRCs match;
  LeanDVB recovers at least 14 contiguous, bit-exact payload frames with zero
  LDPC flips/check failures or BCH corrections/failures. Raw samples use the
  existing waveform; input data gets no station header.
- TS/GCS paced runs, 30 s at 26m667: zero underruns, late fills/switches or fatal
  lateness. Fill <=40137 cycles, slack >=7083 cycles; active encoder mean <=254607
  cycles/frame, live max <=299538. Input timing includes CRC+copy but excludes
  sleeps awaiting bytes; ISR callback cycles are also reported. A separate 2 s
  run pauses its sender for 20 ms/100 ms: dummies must appear, encoding resume,
  and all bank deadlines still pass.
- Wi-Fi start/stop probes with input allocated: no played samples, ESP_OK, both
  free-up and low-water internal RAM >=10000 B. This is a new input experiment's
  gate, lower than the synthetic path's 30000 B target; record the actual reserve.
- Synthetic 11/26m667 16-frame CRC 1271511239 and 13/20ms6 40-frame CRC 3178964763;
  synthetic 30 s timing remains within the same 26m667 gates.

Counters report accepted messages/bytes, NOPs, each rejection reason, sequence loss,
backward/duplicate messages, overruns/restarts, READY falls, ISR count/cycles,
consumed bytes, partial bytes at stop, input calls/cycles/worst call and worst active
input time per completed frame. `spirx` additionally reports verified payload CRC,
wrong payload count, throughput, isolated memcpy maximum and sender counters.
Record real board output under a new `results/<date>-spi-in/`; this branch contains
no invented run records. Re-run external cable/throughput gates when the second S3
arrives. Host checks: `tests/test_ingress.py`, `tests/test_stream.py` and
`tests/test_dvbs2_c.py`; add `--input-only --receiver /path/to/leandvb` to the stream
harness for independent receiver checks on host-generated TS/GCS samples.
