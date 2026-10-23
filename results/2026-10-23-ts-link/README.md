# MPEG transport stream over the S3-to-S3 link: 1.74 million packets exact

One ESP32-S3 sent DVB-S2 transport stream (TS) frames, and another received them. Over 10 runs
the receiver handed on 1,738,344 TS packets of 188 bytes and checked each against the
transmitter's test input. There were no CRC-8 errors, no packets that differed from the input
and no lost packets. The 200,925 frames behind them were all exact against host/dvbs2.py's TS
framing. The runs are 3 at 12.64 Mb/s (8PSK 8/9) and 3 at 6.95 Mb/s (QPSK 3/4), both at
5 Msym/s, plus 1 at 1.39 Mb/s (QPSK 3/4, 1 Msym/s). In 3 more runs the transmitter was
starved of input and sent DVB-S2 dummy frames whenever its input ran dry. The receiver rode
through all 95,810 of them without losing lock or a packet.

Path: transmitter S3 (44:B1:76:B0:E5:54, XIAO) → coax and 30 dB attenuation → receiver S3
(44:B1:76:B0:DA:C8), 2420 MHz, short frames with pilots. Both boards ran c3a1c2d (`info`
records at the top of `runs.jsonl`).
- Transmitter input: `spiinput quad ts replay`, or `spiinput usb ts` fed by the laptop. The
  replay loops 8 presealed PV messages (56 packets) through the real ingress path. For USB, the
  laptop sent 1,316-byte PV TS messages of the counting pattern (`host/spi_input.py pattern`,
  sequence from 0) with `examples/usb-send/send.py`'s `stream()` at a 3 Mb/s ceiling. The
  sender's counters follow each USB run (role `sender`).
- Transmitter: `dvb 2420 MC [5m] 55 0 PEAK --short --phy-solo`, peak 128 at 5 Msym/s and 64 at
  1 Msym/s, keyed 2 s after the receiver started.
- Receiver: `rx 2420 32 45 MC [5m] ts pattern`, checked by `host/rx.py MC --ts replay|live`.
  The receiver stops before the transmitter's data does, as in
  [the 5 Msym/s runs](../2026-10-23-rx-5m-margin/README.md).

## What is checked

- **Frames.** Each frame's data field (its payload at 1 Msym/s, a CRC-32 at 5 Msym/s) equals the
  model frame its sequence number implies. `host/rx.py ts_model` is the transmitter's input
  framed as `host/dvbs2.py ts_bbframes` does (`tests/test_rx_host.py` checks the two agree):
  each sync byte replaced by the CRC-8 of the previous packet, cut into data fields of
  Kbch - 80 bits. So the transmitter's TS framing is bit exact on short frames too.
- **Packets.** The receiver deframes every good frame on the chip (`firmware/components/dvbs2rx/rx_ts.c`):
  - It restores the sync bytes.
  - It checks each packet's CRC-8 against the next packet's sync position.
  - It checks continuity counters per PID.
  - With `pattern`, it compares every byte with the transmitter's test pattern
    (`ingress_pattern_ts_differs`) and checks that each packet is the next one: same message,
    next index, or the next message.
- **The replay's continuity counters jump once per loop.** The loop is 56 packets, which is not
  a multiple of 16, so the 4-bit counter jumps each time it wraps. The receiver counts those
  wraps from the pattern, and its continuity errors equal them exactly. The counting USB input
  has no loop, and there the receiver counted 0 continuity errors.

## Runs

| Mode | Input | Frames exact | Packets | CRC-8 errors | Pattern errors / jumps | Continuity errors | Dummy frames | Pilot MER, lowest / median |
|---|---|---:|---:|---:|---:|---:|---:|---|
| QPSK 3/4, 5 Msym/s, 6.95 Mb/s | replay | 22,993 of 22,993 | 177,828 | 0 | 0 / 0 | 3,175 = loop wraps | 0 | 25.3 / 29.7 dB |
| QPSK 3/4, 5 Msym/s | replay | 23,043 of 23,043 | 178,214 | 0 | 0 / 0 | 3,183 = loop wraps | 0 | 26.3 / 29.7 dB |
| QPSK 3/4, 5 Msym/s | replay | 22,968 of 22,968 | 177,635 | 0 | 0 / 0 | 3,172 = loop wraps | 0 | 27.2 / 29.7 dB |
| 8PSK 8/9, 5 Msym/s, 12.64 Mb/s | replay | 34,313 of 34,313 | 322,870 | 0 | 0 / 0 | 5,765 = loop wraps | 1 | 26.7 / 29.9 dB |
| 8PSK 8/9, 5 Msym/s | replay | 34,402 of 34,402 | 323,707 | 0 | 0 / 0 | 5,780 = loop wraps | 0 | 26.5 / 29.8 dB |
| 8PSK 8/9, 5 Msym/s | replay | 34,390 of 34,390 | 323,595 | 0 | 0 / 0 | 5,778 = loop wraps | 0 | 26.7 / 29.9 dB |
| QPSK 3/4, 1 Msym/s, 1.39 Mb/s | replay | 4,602 of 4,602 | 35,591 | 0 | 0 / 0 | 635 = loop wraps | 0 | 28.5 / 30.3 dB |
| QPSK 3/4, 5 Msym/s | USB, 2.65 Mb/s | 8,638 of 8,638 | 66,806 | 0 | 0 / 0 | 0 | 28,585 | 28.2 / 29.8 dB |
| QPSK 3/4, 5 Msym/s | USB, 2.65 Mb/s | 8,632 of 8,632 | 66,759 | 0 | 0 / 0 | 0 | 28,600 | 28.0 / 29.8 dB |
| 8PSK 8/9, 5 Msym/s | USB, 2.59 Mb/s | 6,944 of 6,944 | 65,339 | 0 | 0 / 0 | 0 | 38,625 | 25.8 / 30.1 dB |

Every run: no frame failed its check or needed the LDPC decoder. No frame was dropped and no
capture unit was lost. The demodulator never relocked. The deframer never restarted, and no
packet went out unchecked.

**Dummy frames.** With USB input the transmitter had data for one frame in 4.6 (QPSK 3/4: 9,796
data frames, 35,498 dummies) or one in 6.9 (8PSK 8/9: 7,887 and 46,862). It sent a dummy
PLFRAME (3,330 symbols, MODCOD 0) whenever its input ran dry. The dummy count is the
receiver's own, from lock to the end of its 40 s. In the first 8PSK replay run the
transmitter's encoder fell behind once (1 underrun) and the receiver rode through that dummy
too.

**Regression.** The last record in `runs.jsonl` is the same receiver without TS: GCS 8PSK 8/9
at 5 Msym/s with the transmitter's synthetic payload. All 34,306 of 34,306 frames were exact,
with 0 dropped, against 34,232 to 34,196 frames in the [earlier runs](../2026-10-23-rx-5m-margin/README.md).

## Cost

**Core 1** is unchanged. The mean per 12,288-sample unit is 117.0k to 117.4k cycles at QPSK 3/4
and 121.0k to 121.2k at 8PSK 8/9, against 117.2k and 120.8k before. The worst unit with capture
service at 5 Msym/s is 161k to 211k cycles, absorbed by one unit of capture lag, as before.

**Core 0.** These are wall-clock cycles per frame from the receiver's counters. They include
preemption by the release task, so they are upper bounds.

| | QPSK 3/4, 5 Msym/s | 8PSK 8/9, 5 Msym/s | QPSK 3/4, 1 Msym/s |
|---|---:|---:|---:|
| Frame period at 240 MHz | 401,760 | 268,704 | 2,008,800 |
| Packets per frame | 7.73 | 9.41 | 7.73 |
| Release | 105.6k (26.3%) | 94.3k (35.1%) | 109.9k (5.5%) |
| Check (re-encode) | 62.6k (15.6%) | 75.3k (28.0%) | 47.0k (2.3%) |
| TS deframer | 14.5k (3.6%), 1.88k per packet | 25.6k (9.5%), 2.72k per packet | 12.0k (0.6%) |
| Pattern check (test only) | 17.5k (4.4%), 2.26k per packet | 32.2k (12.0%), 3.42k per packet | 14.1k (0.7%) |

The per-packet cost depends on how busy the core is. With sparse frames (the USB runs) the
deframer measured 1.22k to 1.24k cycles per packet and the pattern check 1.48k to 1.54k: less
preemption lands inside each call. The deframer's CRC-8 over 187 bytes is about 1k of that (derived: 20 instructions per 4 bytes).

**Memory.** The deframer's state is 712 bytes of internal RAM, taken only in TS mode. Free RAM
after the receiver's allocations was 33,483 bytes at 8PSK 8/9 and 34,475 bytes at QPSK 3/4,
720 bytes below the same modes without TS ([5 Msym/s runs](../2026-10-23-rx-5m-margin/README.md)).
The record's CRC-32 table (1 KB) is in PSRAM.

## Changes behind these runs

- `bda6165` (ride through dummies) and `a6f4762` (ts deframer).
- `7f24d41`. The ROM CRC-32 for the 5 Msym/s record took 30k to 90k cycles per 8PSK frame; a
  byte table gives the same value at a fraction of that. When all three frame slots are busy,
  the release task now waits up to 2 ms for one instead of dropping the frame. Before this,
  frame 2 after lock was dropped in about half of the 8PSK 8/9 TS runs: the first frames come
  in a burst while the first check runs with cold caches (300k to 370k cycles).
- `c3a1c2d` (rx ts mode).

## Not covered

- **Frames the LDPC decoder repairs reach the deframer too late and are given up.** Each one
  costs a deframer restart. None occurred here (MER about 30 dB).
- **The packets went nowhere: the TS egress (`firmware/rx/main/ts_egress.h`) had no sender.**
  Ethernet (W5500) is next.
- **Two earlier USB runs are not listed.** Their host model was too short (8,000 frames for
  9,790 sent), so frames past model frame 8,000 could not match. On the chip they also counted
  0 CRC-8, pattern and continuity errors. The runs listed are their repeats with a longer model.
