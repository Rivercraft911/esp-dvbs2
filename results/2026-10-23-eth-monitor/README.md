# The receiver's monitor stream, at no cost to the video

**Result.** The receiving S3 now sends a live monitor stream (stats five times a second, sample
snapshots and corrected symbols ten times a second; `firmware/rx/main/mon_pack.h`) to the
laptop's dashboard beside the camera video, and the video is as clean as without it: in a
520 s keyed 8 Mb/s camera run with the monitor on, 357,872 frames, 0 frames or demodulator runs
dropped, 0 continuity errors at the receiver, and 19,450 monitor datagrams sent. Two things had
to change: every copy moved to core 0, below the receiver's tasks, and the monitor rides the
two TS sockets' turn instead of a third W5500 socket, which made the chip lose and reorder TS
datagrams.

Setup as in [eth-full-quality](../2026-10-23-eth-full-quality/README.md): receiver
44:B1:76:B0:E5:54 (W5500 at a forced 10 Mb/s), transmitter 44:B1:76:B0:E5:8C (5794cca) fed by
the CM5's pv-capture at 2 x 3.5 Mb/s (8 Mb/s mux), 8PSK 3/4 short, 5 Msym/s, 2420 MHz. The
laptop side went through `host/udp_ts.py --port 1240 --forward 127.0.0.1:1234` (counts and
arrival times) into pv view; the monitor to the dashboard on UDP 1235.

## What the monitor cost the receiver

One minute of the camera run per row (`mon SAMPLES SYMBOLS STATS` sets the rates per second):

| Firmware | Monitor | Datagrams | Demodulator runs dropped | Frames dropped | Continuity errors |
|---|---|---:|---:|---:|---:|
| 6803434 | stats 5 | 394 | 8 | 1 | 8 |
| 6803434 | samples 10, copied on core 1 after a capture unit | 3,124 | 322 | 28 | 280 |
| 6803434 | symbols 10, scaled in the release task | 474 | 106 | 1 | 116 |
| 9be77ae | 5 / 5 / 5, all copies on core 0, sends at priority 3 | 1,420 | 0 | 0 | 0 |
| 9be77ae | 10 / 10 / 5 | 2,425 | 1 | 0 | 1 |
| e6179ce | 10 / 10 / 5, in the TS sockets' turn | 2,452 | 0 | 0 | 0 |

Core 1's copy cost only 3,107 cycles on average (5,105 at most) yet each one cost about 0.4
runs: core 1 has a few percent of slack at 5 Msym/s. Now the sender copies the newest 1,024
samples from the demodulator's int8 ring itself (`rx_demod_snapshot`: a push of up to 3,072
words leaves them alone; a copy that a push overlapped is refused, about 10% of tries), the
release only memcpys up to 320 raw symbols (`rx_demod_tap`), the sender scales them to radius
64 (`rx_demod_tap_scale`), and it drops to priority 3 while it builds and sends a monitor
datagram. Before that, the full monitor at 10 / 10 / 5 cost 4,829 runs and 576 frames in a
470 s run (7b9c480, session-8: the stutter the user saw).

## Whole 520 s runs

| Run | Firmware | Monitor | Receiver: runs / frames dropped, continuity | TS datagrams sent | Laptop socket received | Continuity at the laptop |
|---|---|---|---|---:|---:|---:|
| session-9 | 7b9c480 | off | 1 / 0, 2 | 383,101 | 382,972 | 77 |
| session-12 | 6803434 | stats 2 | 44 / 4, 43 | 383,110 | 383,025 | 80 |
| session-14 | 9af84dc | 10 / 10 / 5 on socket 2 | 15 / 0, 17 | 382,889 | 382,736 | 1,389 |
| session-16 | e6179ce | 10 / 10 / 5 in the TS turn | 0 / 0, 0 | 383,200 | 383,038 | 89 |

With the monitor on its own socket (session-14) the laptop counted 1,389 continuity errors for
153 missing datagrams: while a monitor frame held the wire, both TS sockets could have a SEND
waiting and the W5500 sent them out of order. In the TS turn (`w5500_ts_send_other`: the next
socket in turn, its destination port switched for that SEND and set back) the chip never holds
more than one SEND behind the wire, as with TS alone.

What remains is on the wire: in session-16, 162 of the 383,200 TS datagrams the W5500 confirmed
(0.04%) never reached the laptop's socket, en12 counted no input errors, macOS dropped none for
full socket buffers, and pv view's discontinuities (32, 37, 16 and 4, by PID) add up to the
relay's 89. The other runs lost the same share with the monitor off. The W5500 on the XIAO's
3V3, which cannot hold a 100 Mb/s link, is the likely cause (eth-bringup).

`runs.jsonl`: `live` (receiver summaries), `ab` (the minute runs), `laptop` (udp_ts.py, also
the relays), `tx`, `eth` (`eth` and `mon` commands), `pv` (pv view's /health after session-16).
