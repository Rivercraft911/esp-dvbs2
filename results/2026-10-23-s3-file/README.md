# A file through the S3-to-S3 link

**Result.** An 8.0 MB file went from the laptop into one ESP32-S3 over USB, across the radio
link, and out of the other S3's USB records with its SHA-256 equal. It needed erasure coding
sized for the losses. Each run lost 16 or 17 frames in a row, once or twice, and neither the plain
container nor the 16:4 code could recover from that.

| Run | Coding | Frames sent | Frames exact | Missing | File recovered |
|---|---|---:|---:|---|---|
| `file-1m-qpsk34` | none | 5,513 | 5,496 | 16, in one run | no: truncated container |
| `file-1m-qpsk34-fec16-4` | 16:4 | 7,172 | 7,138 | 33, in runs of 16 and 17 | no: block 14 had 4 of 20 |
| `file-1m-qpsk34-fec96-32` | 96:32 | 7,692 | 7,658 | 33, in runs of 17 and 16 | yes, SHA-256 equal |

- Every frame that arrived was exact, byte for byte against the bytes the transmitter was given.
  None failed its check or needed the LDPC decoder.
- The frames were not lost on the radio. The receiver's demodulator saw all of them (0 lost) and
  never relocked. They were dropped at its USB record write, which blocked for 1.39 to 1.41 s
  while the laptop stopped reading.
- In the same second the laptop also stopped writing to the transmitter. Its USB input waited
  1.10 to 1.11 s, and it sent dummy frames meanwhile. The two boards are on separate USB buses of
  the laptop, so the pause was in the laptop, not in either board.
- The frame before lock (frame 0, acquisition zeros) is the only other frame not received.

The file is `git archive --format=tar.gz 517c641` of this repository: 7,998,088 bytes, SHA-256
`782e7420a17adf95efaf028be4e2a511dbd4ef8cb80b9fe7d188704d49f23d2a`.

## Setup

- **Boards.** Transmitter 44:B1:76:B0:E5:54 (XIAO) and receiver 44:B1:76:B0:DA:C8, both on
  517c641 (main), from their `info` records. The ports were found by USB serial number, which is
  the MAC.
- **Link.** Coax and 30 dB of attenuation, 2395 MHz, QPSK 3/4 short frames with pilots at
  1 Msym/s: 1,454 bytes a frame, 1.39 Mb/s. Transmitter peak 64, gain 0. Receiver gain 32.
- **Laptop.** macOS (Darwin 24.6.0), `/usr/local/bin/python3`. Another session ran a B200 on the
  same laptop during these runs (see [the pauses](#the-pauses)).

## Method

The receiver's record carries each good frame's whole data field at 1 Msym/s
([receiver](../../docs/receiver.md)). So the file can come out of the records.

1. **What the transmitter is given.** `examples/usb-send/send.py`'s `container()` builds the byte
   stream: 8 zero-filled frames for acquisition, the `PVFILE1` header with the name, length and
   SHA-256, the file, padding to a whole frame and 4 more frames. With FEC each frame holds one
   `host/filelink.py` packet of 1,454 bytes, a 1,398-byte shard and 56 bytes of header and CRC
   ([file link](../../docs/file-link.md)). The stream is saved before sending.
2. **Transmitter.** `spiinput usb gcs`, then `dvb 2395 7 SECONDS 0 64 --short` with 70, 82 or
   90 s. `send.py`'s `stream()` sends the saved stream as PV messages with USB backpressure and no
   rate ceiling, from 2 s after the receiver starts.
3. **Receiver.** `host/rx.py 7 --mhz 2395 --seconds N --input SENT --save GOT` (N = 62, 74, 84):
   `rx 2395 32 N 7`. `--input` makes the model the saved stream cut into 1,454-byte fields, so
   each frame is checked against the bytes the transmitter was given. `--save` writes the exact
   frames' data fields in sequence order.
4. **Recovery.** `send.py unpack GOT [--fec] -o recovered.tar.gz`, then SHA-256 against the
   original. `unpack` writes nothing unless the hash matches.

The command line `send.py send FILE --short --modcod 7 --fec 96:32 --key --frequency 2395
--seconds 60` does steps 1 and 2 in one go, at peak 128 and at most 60 s. These runs drove
`stream()` from a script to use peak 64 and up to 90 s.

## Runs

| Run | Stream sent | Frames | Missing runs (first frame, length, s into the run) | Slowest record write | Pilot MER lowest / median | Laptop to transmitter |
|---|---:|---:|---|---:|---|---|
| plain | 8,015,902 B | 5,513 | 1348, 16, 11.3 s | 1.387 s | 28.2 / 30.6 dB | 1.35 Mb/s |
| 16:4 | 10,428,088 B | 7,172 | 289, 16, 2.4 s; 5757, 17, 48.2 s | 1.414 s | 28.5 / 30.6 dB | 1.33 Mb/s |
| 96:32 | 11,184,168 B | 7,692 | 1107, 17, 9.3 s; 6572, 16, 55.0 s | 1.404 s | 27.9 / 30.6 dB | 1.33 Mb/s |

"Frames" is what the transmitter encoded, which equals the stream divided by 1,454. In each run
the receiver's `frames_dropped` equals the missing frames; its demodulator counted every frame,
with 0 lost, 0 dropped samples and 0 relocks. The laptop rate is `send.py`'s steady payload rate
after the opening ID. The heap counter was 0 allocations after boot on both boards before and
after every run.

**The codes.** 16:4 survives any 4 missing frames in each block of 20. The first burst took 16 of
block 14's 20 frames. 96:32 survives 32 missing in each block of 128 (1.07 s at this frame rate),
and neither burst took more than 17 from one block. Its cost: 128 × 1,454 / (96 × 1,398) - 1 =
38.7% more bytes, against 30.0% for 16:4, so 1.00 Mb/s of file at 1.39 Mb/s of payload.

## The pauses

| Run | Receiver: slowest write, slow writes (1 ms or more) | Transmitter: seconds after ready with input below 80% of the median | Longest input wait |
|---|---|---|---:|
| plain | 1.387 s at frame 1346, 2 | 16 and 17 | 263.7M cycles, 1.10 s |
| 16:4 | 1.414 s at frame 5755, 5 | 7 and 8, 54 and 55 | 267.0M cycles, 1.11 s |
| 96:32 | 1.404 s at frame 1105, 5 | 14 and 15, 61 and 62 | 266.1M cycles, 1.11 s |

The transmitter's per-second USB trace (`usb.trace`) dips in the seconds the receiver loses
frames, and in two runs a second pause came 47 s after the first. The dip at the end of each
transmitter trace, where the file runs out, is not a pause.

What I can say about the cause:

- **The laptop, not the boards.** The transmitter is on a hub behind a USB 3.1 bus and the
  receiver alone on another. A pause in both at once lies upstream of both.
- **It began during this session.** In the first two TS runs just before
  ([live TS](../2026-10-23-s3-live-ts/README.md)), 60 s each, neither board saw a pause: slowest
  record write 51 and 73 µs, longest input wait 0.06 and 0.04 s. The third TS run had a 1.11 s
  input wait on the transmitter. Its receiver lost nothing: at 5 Msym/s a record is 24 bytes, and
  the receiver's 8 KB USB buffer held them until the laptop read again.
- **The B200 is a candidate, not established.** Another session used a B200 on the same laptop
  over this period. I did not stop it to test this.
- **It matches the bursts that are not settled.** The receiver page records one burst of about
  950 frames (1.06 s) in 12 of 27 runs of one session, blamed on the USB record write without
  proof ([receiver](../../docs/receiver.md),
  [Wi-Fi A/B](../2026-10-23-rx-no-wifi-ab/README.md#not-settled)). Here a write is seen to block
  for 1.4 s while the other board's input stops in the same second.

## Changes behind these runs (b1ac58b)

- `host/rx.py --input FILE` checks frames against a real input: a TS file with `--ts live`, or the
  GCS bytes. The run is placed by its first frame whose data field occurs once in the model,
  because a stream that opens with 8 identical zero frames would otherwise be placed at frame 0.
- `host/rx.py --save FILE` writes the exact frames' data fields at 1 Msym/s.
- `send.py --short` sends short frames and sizes the container and FEC packets to them.
- Tests: `tests/test_rx_host.py` (the input model against `ts_model`, placement past repeated
  frames) and `tests/test_usb_send.py` (short-frame sizes and commands).

## Records

`runs.jsonl`, by `set`: the receiver's summary (role `rx`), the transmitter's records (role `tx`,
with `usb.trace`), the sender's counters (role `sender`, with the stream's size), `info` from both
boards before and after (role `info`), and the recovery (role `unpack`).
