# Quad SPI payload input, radio off

The S3 now takes its payload over quad SPI: DMA into internal RAM, a READY line for back-pressure,
MPEG-TS and generic-stream framing with CRC and sequence checks, and dummy frames when input runs
dry. On the board it passes every functional check, including the demo path (SPI input ->
transmitter -> dump -> bit-exact LeanDVB decode -> paced run) and 8PSK 2/3 at 10 Msym/s with
input running, which starved in earlier rounds. One Wi-Fi heap probe misses its floor. No RF.

Firmware `b9eb7d7` (exp/spi-in), ESP32-S3 at 240 MHz, one-board loopback: the second SPI
peripheral of the same chip plays the sender through the GPIO matrix, no jumpers. `board.log` is
the full `SECONDS_PER_RUN=15 bash host/spi-in-board.sh` run; docs/spi-input.md has the protocol,
pins (XIAO default: CLK GPIO7, CS GPIO1, data GPIO2/4/5/6, READY GPIO8) and a CM5 sender recipe
(Linux spidev, mode 0, single-line 25 MHz).

| Check | Result |
|---|---|
| Demo: TS in over SPI, dump decodes, paced run | pass |
| Throughput, verified payload | 22 and 46.6 Mb/s pass; offered 87 Mb/s reaches about 57.7 Mb/s on the one-board loopback |
| Injected faults 1-5 (CRC, short, long, header, sync) | all detected |
| Starvation (input stops) | dummy frames, no missed bank |
| Regressions (QPSK, 8PSK, paced) | pass |
| `wifimem 13 10m` with input | **fail**: 20,799 B free with Wi-Fi up, low-water 18,875 B, floor 20,000 |

Round history: round two found a loopback sender at task priority 6 preempting the priority-5
encoder (8PSK 10 Msym/s starved, 454k cycles per frame); round four lowered it and restored the
byte-wise BCH that input mode had disabled. Runs before the coordinator's power cycle at 04:2x on
2026-10-17 were invalidated by overlapping board jobs and are not used.

Not shown: an external sender (the CM5 or a second S3), and anything above about 58 Mb/s, which
the one-board loopback cannot prove because the same chip also plays the sender.
