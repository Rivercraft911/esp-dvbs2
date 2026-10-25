# Live demo: 2.4 hours of camera video from one S3 to another

**Result.** For the live demo, the whole chain ran in 16 back-to-back keyed runs of 540 s, 2.4
hours on air. Two cameras on a CM5 made an 8 Mb/s MPEG transport stream, SPI carried it into the
transmitting S3, and 8PSK 3/4 short frames at 5 Msym/s carried it through coax to the receiving
S3, its W5500 and the laptop's viewer. The receiver decoded 5,725,408 frames with 0 failed, a
median pilot MER of 25.4 to 25.6 dB per run, no relocks and no capture drops. It lost 11 frames
in five runs when its release task fell behind, about one every 13 minutes. Its W5500 took
6,127,597 datagrams with none dropped and no SPI underflow, and confirmed all but one at the end
of run 28, while the monitor stream fed the dashboard. The cameras kept 30.0 frames/s each, and the
receiver made no heap call after boot.

## Setup

- **Source.** A Raspberry Pi CM5 running `pv-capture`: two IMX900 cameras at 2064 x 1552 and
  30 frames/s, H.264 at 3.5 Mb/s each in an 8 Mb/s mux, sent over single-line SPI at 16 MHz. At
  4 Mb/s per camera `pv-capture` drops frames itself
  ([full quality](../2026-10-23-eth-full-quality/README.md)), so the demo used 3.5.
- **Transmitter.** XIAO ESP32-S3 44:B1:76:B0:E5:8C, firmware 5794cca: `spipins xiao`,
  `spiinput on ts`, then `dvb 2420 14 5m 540 0 128 --short` (8PSK 3/4, short frames with pilots,
  roll-off 0.35, 10.39 Mb/s of capacity) through `host/board.py ... --monitor 127.0.0.1:1235`,
  which also passed its status lines to the dashboard.
- **Path.** 2420 MHz, coax and 30 dB of attenuation.
- **Receiver.** XIAO ESP32-S3 44:B1:76:B0:E5:54, firmware 3a1783d:
  `rx 2420 32 548 14 5m ts nonull eth` (575 s in runs 19 to 21), the W5500 on the direct GP-SPI2
  backend and forced to 10 Mb/s full duplex (`eth phy 10f`), the monitor stream on.
- **Laptop.** `host/udp_ts.py --port 1240 --forward 127.0.0.1:1234` relayed the stream to
  PigeonVision's `pv view`, which stitched the two fisheye cameras into a 360° view, and
  `host/monitor.py` ran the dashboard.

Both firmware versions are from `board.py info` after the last run; the transmitter's uptime,
5.6 hours, covers every run. Each receiver run was read with `host/rx.py`'s `receive()` and kept
without a model check, since live camera video has no model to check against.

## Runs

| Run | Ended, UTC | Frames good | Dropped | Pilot MER, p10 / median, dB | Continuity errors | Datagrams | SPI messages missed | Camera frames, A / B |
|---:|---|---:|---:|---|---:|---:|---:|---|
| 19 | 02:28 | 357,862 | 1 | 24.9 / 25.6 | 2 | 382,983 | 0 | 15,597 / 15,592 |
| 20 | 02:38 | 357,788 | 0 | 25.0 / 25.5 | 0 | 383,036 | 0 | 15,593 / 15,590 |
| 21 | 02:50 | 357,813 | 2 | 24.9 / 25.6 | 11 | 382,975 | 27 | 15,600 / 15,590 |
| 22 | 03:02 | 357,865 | 1 | 24.9 / 25.6 | 2 | 383,004 | 0 | 15,596 / 15,581 |
| 23 | 03:11 | 357,803 | 0 | 24.9 / 25.5 | 0 | 382,918 | 0 | 15,592 / 15,594 |
| 24 | 03:20 | 357,849 | 0 | 24.8 / 25.4 | 0 | 383,045 | 0 | 15,596 / 15,591 |
| 25 | 03:29 | 357,879 | 0 | 24.9 / 25.5 | 0 | 382,901 | 0 | 15,587 / 15,586 |
| 26 | 03:39 | 357,868 | 0 | 25.0 / 25.5 | 0 | 382,913 | 0 | 15,588 / 15,593 |
| 27 | 03:48 | 357,834 | 0 | 25.0 / 25.5 | 0 | 382,934 | 0 | 15,596 / 15,590 |
| 28 | 03:57 | 357,824 | 0 | 25.0 / 25.5 | 0 | 382,903 | 0 | 15,590 / 15,583 |
| 29 | 04:06 | 357,809 | 1 | 24.9 / 25.6 | 1 | 383,071 | 0 | 15,599 / 15,589 |
| 30 | 04:15 | 357,812 | 0 | 24.8 / 25.5 | 0 | 382,914 | 0 | 15,595 / 15,582 |
| 31 | 04:24 | 357,811 | 0 | 24.9 / 25.6 | 1 | 383,005 | 0 | 15,597 / 15,590 |
| 32 | 04:34 | 357,832 | 0 | 24.8 / 25.6 | 0 | 382,958 | 0 | 15,591 / 15,594 |
| 33 | 04:43 | 357,896 | 0 | 25.0 / 25.5 | 0 | 383,029 | 0 | 15,593 / 15,599 |
| 34 | 04:52 | 357,863 | 6 | 24.9 / 25.5 | 6 | 383,008 | 0 | 15,591 / 15,588 |

Run numbers are the CM5's session numbers. **Dropped** is the demodulator's count of frames lost
because the release fell behind (`dvbs2rx.h`); failed frames would be a separate count, and there
were none. **Continuity errors** are the receiver's own TS check, before the W5500.
**Datagrams** are the TS datagrams handed to the W5500; the monitor stream's 302,499 more are not
counted, and 139 monitor datagrams were skipped by design when they could not go out in their
period. The chip confirmed every datagram (SEND_OK) except one in run 28, whose egress ended with
status `full` and kept the socket's registers in its record. Its queue never held more than 60
packets and dropped none, so this happened at the end of the run, not partway through.
**SPI messages missed** are the transmitter's input counters: sequence numbers skipped plus
messages too short to accept.

The transmitter encoded 5,725,502 data frames and filled the rest of its 10.39 Mb/s with
2,989,029 dummy frames, with no late fill or switch in any run. Its encoder averaged at most
81.1k cycles a frame, 30% of the 1.12 ms frame period. It accepted 6,325,915 SPI messages, an
8.00 Mb/s payload in each run, and the CM5's transport dropped nothing.

## Where the errors come from

- **The receiver's release.** All 11 dropped frames were lost on the receiving S3: core 0 runs the
  release, the TS check, the W5500 sender and the monitor copies, and in five runs the release
  fell behind for a frame or a few. Every such frame shows up as continuity errors.
- **The transmitter's input.** In run 21 the transmitter missed 27 SPI messages from the CM5
  (10 skipped, 17 short); with its 2 dropped frames that gave 11 continuity errors. Of the 23
  continuity errors in all, 22 fall in the five runs with dropped frames. The one in run 31
  is the frame the LDPC decoder repaired there (`decoded` 1, 2 iterations): in TS mode a repaired
  frame arrives too late for the stream and its packets are dropped
  ([receiver](../../docs/receiver.md#transport-streams)).
- **The cameras.** `pv-capture` dropped 2 to 19 frames per camera per run as
  `capture_queue_full`, 274 of about 499,200, plus one `camera_frame_error` per camera as each
  run started.
- **Between the W5500 and the laptop.** The relay kept one summary for its whole 2 hours, which
  spans runs 22 to 33, parts of runs 21 and 34 and the pauses between them: 4,773,020 datagrams,
  33,394,579 TS packets, 87,229 continuity errors (0.26% of packets), 11 sync errors, no
  transport errors flagged, arrival gaps of 1.12 ms median and 9.42 ms at the 99.9th percentile.
  The W5500 confirmed the datagrams it sent, so these losses happened on the wire or in the
  laptop. The summary cannot split them by run, and at times in this window the laptop was
  also encoding video for the README clip. Read twice 10 s apart during run 23, the viewer showed
  30.3 frames/s for each camera and no new discontinuities.

## What this does not show

The path is coax with fixed attenuation, so nothing here says anything about range. It is one
pair of boards at one carrier and one gain. Picture quality on the laptop was watched, not
measured. Loss on the Ethernet side is known only for the relay's 2 hours as a whole.

`runs.jsonl`: `info` (both boards after the last run), and per run `live` (the receiver's host
summary with the board's records), `tx` (the transmitter's summary) and `cm5` (`pv-capture`'s
frames, health and configuration); then `laptop` (the relay's summary) and `pv` (the viewer's
`/health` during run 23, as printed).
