# Full-quality camera video through the receiver's W5500 at 10 Mb/s

**Result.** The full camera mux (two IMX900 at 4 Mb/s each, 9 Mb/s CBR, about 8.8 Mb/s of
TS once its null packets are left out) now leaves the receiving S3 over a forced 10 Mb/s link
with no egress drops: in a 540 s keyed run 432,975 datagrams, every one confirmed by the chip,
0 packets dropped at the egress queue (peak 162 of 4,096) and 0 SPI underflows, with all
402,607 frames good. The egress sustains 9.46 Mb/s of TS under live receive, the wire's ceiling,
by sending on two W5500 sockets in turn. What still shows in the viewer comes from elsewhere:
0.09% of datagrams lost between the W5500 and the laptop, and camera frames that pv-capture
drops itself at 4 Mb/s per camera.

Setup as in [eth-bringup](../2026-10-23-eth-bringup/README.md): receiver XIAO 44:B1:76:B0:E5:54
with a W5500 on the XIAO's 3V3 (forced 10 Mb/s full duplex, `eth phy 10f`), the direct GP-SPI2
backend at 26.67 MHz, `rx 2420 32 575 14 5m ts nonull eth`; transmitter 44:B1:76:B0:E5:8C
(firmware 5794cca, exp/tx-status) with `spiinput on ts` from the CM5's `pv-capture` over
single-line SPI at 16 MHz, 8PSK 3/4 short frames at 5 Msym/s (10.39 Mb/s), 2420 MHz, coax.
Laptop: UGREEN AX88179B adapter at 192.168.1.100, PigeonVision `pv view` on UDP 1234.

## What limited the egress, and the fixes

Full-load tests: the transmitter's TS replay pattern (all non-null, 10.39 Mb/s, more than the
wire carries), 30 s keyed, `host/rx.py 14 --rate 5m --ts replay --eth`, every packet checked
byte for byte at the laptop (`host/udp_ts.py --pattern replay`). Drain is the egress's TS rate
while two datagrams' worth or more waited; it means something only when that lasts (20 s here).

| Firmware | Change | Drain | Underflows | Egress | Demodulator runs lost |
|---|---|---:|---:|---|---:|
| e5b7f9b | 4,096-packet queue (from 256) | 9.04 Mb/s (1.2 s) | 5 | stopped: a write underran past 3 rewrites | 32 |
| 3b8b09e | 10 rewrites, SPI at 20 MHz | 8.98 Mb/s (1.1 s) | 12 | stopped the same way | 25 |
| a00fdd8 | GDMA burst for word-aligned writes, as ESP-IDF's spi_master sets it | 8.94 Mb/s | 0 | ok | 356 |
| 9589df2 | wake 40 us early, spin up to 120 us for SEND_OK | 9.37 Mb/s | 15 | ok | 1,579 |
| 142d22f | 15 us early, 40 us spin | 9.18 Mb/s (6.7 s) | 23 | stopped | 625 |
| f0dc11e | spin reverted; wait for GDMA data before SPI_USR; reset GP-SPI2 and the GDMA after 10 underflows in a row | 8.94 Mb/s | 0 | ok | 484 |
| b2d43db | release task above the sender (14 against 13) | 8.59 Mb/s | 0 | ok | 235 |
| 7ccac7d | two sockets in turn | **9.46 Mb/s** | 0 | ok | 159 |

The laptop saw every packet it received byte-exact in each run (rewrites catch an underflow
before its SEND). Underflows (zeros, then the payload's first byte repeated: an SPI write shifted
out of an empty FIFO) went from every run to none with GDMA burst and the wait for data before
SPI_USR; a camera run with burst alone (run 4) still had 23. The W5500 sends one datagram per socket at a time and starts the
next only when the driver is called, so with one socket the wire waited for the sender to wake
(about 70 us per 1.106 ms datagram); spinning for SEND_OK above the receiver's tasks cost the
demodulator runs. With two sockets datagram k gets its SEND while k - 1 is on the wire, and the
chip sends it straight after (fake chip: 40 datagrams in 42.2 wire times, against 49.5 to 52.8
with one socket, for a sender that comes back every 1.0 ms). Under this saturated test the
demodulator still loses runs (159, 0.9% of frames); in the camera runs it lost 0 to 30.

## Live camera runs

| Run | Firmware | Mux | Frames | Egress | Laptop (pv view) |
|---|---|---|---|---|---|
| 4 | f322600 | 9 Mb/s | 402,573 good | stopped after 30 s: a write underran past its rewrites | video stopped |
| 5 | b2d43db | 9 Mb/s | 402,598 good, 0 runs lost | 8.61 Mb/s, backlogged 520 s, 52,077 packets dropped (1.7%), peak 4,096 | 10,559 and 10,452 discontinuities, 8,075 corrupt |
| 6 | b2d43db | 6 Mb/s | 268,387 good, 0 lost | 282,127 datagrams, 0 dropped, peak 14 | 213 datagrams short; 4 and 3 discontinuities, 10 corrupt |
| 7 | 7ccac7d | 9 Mb/s | 402,607 good, 30 runs lost | 432,975 datagrams, 0 dropped, peak 162 | 379 datagrams short of the socket; 141 and 98 discontinuities, 155 corrupt |

All 540 s keyed, no capture drops, no heap after boot, pilot MER about 25 dB, transmitter
without late fills and with every SPI message from the CM5 (CRC 0, lost 0). Run 7's laptop
side went through a relay (`udp_ts.py --port 1240 --forward 127.0.0.1:1234`): arrival gaps
median 1.11 ms, 99th percentile 3.89 ms, 99.9th 8.25 ms, longest 27.7 ms, none over 100 ms.

## Where the remaining stutter comes from

- **The wire.** 379 of 432,975 confirmed datagrams (0.09%) never reached the laptop's socket in
  run 7, 213 in run 6; macOS dropped none for full socket buffers and en12 counted no input
  errors. Each loss breaks a picture until its next keyframe. Most likely the W5500 sends
  damaged frames now and then: the same supply that keeps it off 100 Mb/s
  ([eth-bringup](../2026-10-23-eth-bringup/README.md#link)).
- **pv-capture at 4 Mb/s per camera.** In sessions 3, 4 and 6 it dropped 8 to 15 frames per
  camera as `capture_queue_full` (18 to 34 sensor gaps over 50 ms, up to 267 ms); at 2.5 Mb/s
  (session 5) none. Its transport dropped nothing.
- **The source stream.** The receiver's deframer counted 40 continuity errors in run 7 with
  every frame good and every SPI message received: they are in pv-capture's TS.

`runs.jsonl`: `drain` (rx.py summaries), `live` (receiver summaries), `laptop` (udp_ts.py),
`tx`, `replay` (radio-off replays of the same session), `cm5` (pv-capture's frames.jsonl and
health.jsonl per session) and `pv` (the viewer's /health at the end of runs 5 to 7, as printed).
