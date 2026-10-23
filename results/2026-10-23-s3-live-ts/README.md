# Live TS from the laptop through the S3-to-S3 link

**Result.** A real H.264 transport stream made by ffmpeg went from the laptop over USB into one
ESP32-S3, across the radio link, and was deframed on the other S3. Every frame the receiver
decoded was exact against the file, and every TS packet passed its CRC-8 with no continuity
errors.

The long run sent 540 s of a live 2.4 Mb/s stream at QPSK 3/4, 5 Msym/s. The receiver deframed
819,042 packets from 105,901 frames, all exact. It found no CRC-8 error, no continuity error and
no lost frame.

| Run | Mode | Input | Frames exact | Packets deframed | CRC-8 / TEI | CC errors | Lost frames | Dummy frames | Pilot MER lowest / median |
|---|---|---|---:|---:|---:|---:|---:|---:|---|
| `ts-5m-qpsk34` | QPSK 3/4, 5 Msym/s, 6.95 Mb/s | paced at 2.40 Mb/s | 11,052 of 11,052 | 85,476 | 0 / 0 | 0 | 0 | 52,679 | 28.3 / 30.3 dB |
| `ts-1m-qpsk89` | QPSK 8/9, 1 Msym/s, 1.69 Mb/s | USB backpressure, 1.69 Mb/s | 6,391 of 6,391 | 60,136 | 0 / 0 | 0 | 0 | 0 | 28.4 / 30.7 dB |
| `ts-5m-qpsk34-unpaced` | QPSK 3/4, 5 Msym/s | USB backpressure, 2.93 Mb/s | 13,803 of 13,803 | 106,752 | 0 / 0 | 0 | 0 | 45,669 | 27.7 / 29.9 dB |
| `ts-5m-qpsk34-540s` | QPSK 3/4, 5 Msym/s | paced at 2.40 Mb/s, 540 s | 105,901 of 105,901 | 819,042 | 0 / 0 | 0 | 0 | 518,315 | 27.4 / 30.1 dB |

- **The fastest short-frame mode USB keeps fed is QPSK 8/9 at 1 Msym/s.** With no rate ceiling
  the laptop delivered 1.69 Mb/s, the mode's whole payload rate, and the receiver saw no dummy
  frame. At 5 Msym/s USB tops out at 2.93 Mb/s, below the slowest short mode there (QPSK 1/2,
  4.15 Mb/s), so every 5 Msym/s run carries dummy frames.
- **The TS came out byte for byte.** At 1 Msym/s the records carry whole data fields. Rebuilt on
  the laptop, the 60,136 packets equal the file from byte offset 1,880 on (SHA-256
  `73b4b834...ec4e55` both). ffprobe reads 1,364 video frames from them.
- **TEI.** The receiver sets the transport_error_indicator on a packet that fails its CRC-8, so
  the TEI count equals the CRC-8 errors. The ffmpeg files carry no TEI (checked on the laptop).

## Setup

- **Boards.** Transmitter 44:B1:76:B0:E5:54 (XIAO) and receiver 44:B1:76:B0:DA:C8, both on
  517c641 (main), from their `info` records. The ports were found by USB serial number, which is
  the MAC.
- **Link.** Coax and 30 dB of attenuation, 2395 MHz, short frames with pilots. Transmitter
  peak 128 at 5 Msym/s and 64 at 1 Msym/s, gain 0. Receiver gain 32.
- **Input.** Two files from ffmpeg 9.0.2, `testsrc` at 640x360 and 25 frames/s with two decimals
  on its counter, so the number on screen changes every frame (it reads 2200 at 22.00 s).
  H.264 (libx264, veryfast, GOP 50) in constant bit rate (`nal-hrd=cbr`), in an MPEG-TS with a
  fixed `-muxrate` padded with null packets:

  | File | Mux rate | Video | Length | SHA-256 |
  |---|---:|---:|---:|---|
  | `live-2m4.ts` | 2.4 Mb/s | 1.6 Mb/s | 600 s, 179,996,276 B | `6b4db3f2...40ef2c` |
  | `live-1m6.ts` | 1.6 Mb/s | 1.1 Mb/s | 180 s, 35,997,864 B | `96b1bc05...7c7299` |

  ```sh
  ffmpeg -f lavfi -i testsrc=size=640x360:rate=25:decimals=2 -t 600 -c:v libx264 -preset veryfast \
    -pix_fmt yuv420p -g 50 -b:v 1600k -maxrate 1600k -bufsize 1600k -x264-params nal-hrd=cbr \
    -f mpegts -muxrate 2400000 live-2m4.ts
  ```

  PIDs: 0x100 video, 0 (PAT), 0x1000 (PMT), 0x11 (SDT) and 0x1FFF (null).

## Method

1. **Transmitter.** `spiinput usb ts`, then `dvb 2395 MC [5m] SECONDS 0 PEAK --short`.
   `examples/usb-send/send.py`'s `stream()` sends the file from its first byte as 1,316-byte PV TS
   messages, from 2 s after the receiver starts. `ts-5m-qpsk34` and the long run are paced at the
   mux rate, 2.4 Mb/s, as a live encoder would deliver the stream. The other two have no ceiling:
   USB backpressure sets the rate.
2. **Receiver.** `host/rx.py MC [--rate 5m] --mhz 2395 --seconds N --ts live --no-pattern --input
   FILE`, which runs `rx 2395 32 N MC [5m] ts`. The model is the file framed as the transmitter
   frames TS: each sync byte replaced by the CRC-8 of the packet before, cut into data fields
   (`host/rx.py input_model`, checked against `ts_model` in `tests/test_rx_host.py`). Each frame's
   payload (1 Msym/s) or CRC-32 (5 Msym/s) must equal the model field its sequence number implies.
   The receiver deframes every good frame on the chip and counts packets, CRC-8 errors and
   continuity errors per PID (null packets aside). `rx.py` then requires every frame from the
   first decoded to the last present and exact, and the packet count equal to the packets those
   frames hold.
3. **Rebuild.** For `ts-1m-qpsk89`, `rx.py --save` kept the data fields. Cut at the first packet
   start and with sync bytes set to 0x47, they are compared with the file.

**Sent against deframed.** The receiver stops before the transmitter's data does, so it deframes
fewer packets than the laptop sends. In each run the transmitter took the laptop's bytes and
cut them into whole data fields (a partial last field is not sent), and the receiver has every
frame from the first after lock to its last. In the long run the last 4 messages (28 packets)
arrived as the transmitter stopped taking input, so it took 823,193 of the 823,221 packets.

| Run | Packets sent | Frames encoded | Frames before lock | Frames after the receiver stopped | Packets deframed |
|---|---:|---:|---:|---:|---:|
| `ts-5m-qpsk34` | 92,554 | 11,967 | 4 | 911 | 85,476 |
| `ts-1m-qpsk89` | 65,254 | 6,934 | 1 | 542 | 60,136 |
| `ts-5m-qpsk34-unpaced` | 115,199 | 14,895 | 4 | 1,088 | 106,752 |
| `ts-5m-qpsk34-540s` | 823,221 | 106,437 | 2 | 534 | 819,042 |

The receiver also lost no capture samples, dropped no frame, never relocked, and no frame needed
the LDPC decoder. The deframer never restarted. `unchecked` was 0, or 1 where the run ended on a
packet whose CRC-8 never came. The heap counter was 0 allocations after boot on both boards
before and after every run.

## The 540 s run

- **Window.** `dvb 2395 7 5m 540 0 128 --short`, with the receiver on for 530 s from 2 s before
  keying. The receiver saw about 523 s of the stream: 819,042 packets, 2.35 Mb/s on average.
- **Dummy frames.** The transmitter sent 106,437 data frames and 520,852 dummy frames; the
  receiver rode through 518,315 of the dummies without a relock.
- **MER.** Pilot MER was 27.4 dB at its lowest, median 30.1 dB. `rx.py`'s per-minute MER counts
  only data frames, 177 s of them here, so its three "minutes" are thirds of the run, not minutes:
  medians 30.0, 30.2 and 30.2 dB.
- **Load.** Per frame on core 0: release 106.8k cycles, check 46.8k, TS deframer 10.1k (1.30k
  per packet). Core 1's demodulator work came to 123.6k cycles per 12,288-sample unit
  (`push_cycles` over `units`; a unit lasts 184,320), and the slowest unit with capture service
  took 170.6k. Wall-clock counters, as in
  [ts-link](../2026-10-23-ts-link/README.md).

**The laptop's USB pauses did not cost a packet.** The pauses in
[the file runs](../2026-10-23-s3-file/README.md#the-pauses) stop both boards' USB at once for
about 1.1 s. Here the transmitter's longest wait for input was 1.14 s (272.7M cycles). Its USB
trace covers only the first 64 s, so the pauses cannot be placed or counted directly. Derived
from the sender: paced at 2.4 Mb/s for 529.3 s, less its longest write (5.8 s, which holds the
opening ID), it should have sent 157.0 MB and sent 154.8 MB. So about 7.6 s of sending was lost,
about seven 1.1 s pauses. The 60 s paced run lost 0.02 s by the same sum. During a pause the
transmitter sends dummy frames, and the receiver lost nothing: at 5 Msym/s a record is 24 bytes,
and its slowest USB write was 50 µs. The stream comes out about 1.1 s late after each pause, with every packet. At 1 Msym/s,
where a record carries 1,474 bytes, the same pauses dropped 16 or 17 frames.

## Changes behind these runs (b1ac58b)

- `host/rx.py --input FILE` checks frames against a real input: with `--ts live`, a TS file. The
  run is placed by its first frame whose data field occurs once in the model.
- `host/rx.py --save FILE` writes the exact frames' data fields at 1 Msym/s.
- `send.py --short` sends short frames.

## Records

`runs.jsonl`, by `set`: the receiver's summary (role `rx`), the transmitter's records (role `tx`,
with `usb.trace`, the bytes taken per second), the sender's counters (role `sender`), `info` from
both boards before and after each run (role `info`), and the rebuild check (role `host-ts`).
