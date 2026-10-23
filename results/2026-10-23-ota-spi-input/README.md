# Live SPI input from a CM5, over the air

**Result.** A Raspberry Pi CM5 fed the transmitter over single-line SPI in five keyed runs, and
LeanDVB decoded every one from a continuous B200 capture of the whole run. Every recovered TS
packet, GCS frame and file shard was exact and in order, with no gap and no duplicate inside the
data. The S3's SPI counters matched the sender's in every run: messages, payload bytes, next
sequence and, for TS, the CRC chain. No SPI message was rejected.

- **8PSK 2/3, 5 Msym/s, H.264 TS at 9.68 Mb/s**: 257,131 of 257,381 packets, exact and
  contiguous. The sender ran at the mode's rate, 9.66 Mb/s with READY setting the pace, and the
  S3 sent no dummy frame while data flowed.
- **QPSK 3/4, 10 Msym/s, TS at 14.52 Mb/s**: 385,852 of 386,148 packets, exact and contiguous.
  No dummy frame while data flowed.
- **Highest single-line rate**: the CM5's Python sender reached **16.36 Mb/s** at a 20 MHz
  clock (the ideal is 19.4 Mb/s). Into QPSK 9/10 at 10 Msym/s (17.46 Mb/s) that delivered
  425,174 of 425,514 packets, exact and contiguous. The S3 filled the shortfall with 7,178
  dummy frames, 2.4 s of the 39 s data stretch.
- **A 16.2 MB file with `--fec 16:4`** (8PSK 2/3, 5 Msym/s): 3,818 of 3,820 shards arrived,
  and `unpack --fec` rebuilt the file with the same SHA-256.
- **Live VCM controls over SPI** (mixed QPSK and 8PSK at 5 Msym/s): 5,403 frames,
  30,844,632 bytes exact and contiguous across four switches, QPSK 1/2 to 8PSK 2/3 to
  8PSK 9/10 to QPSK 9/10 to QPSK 1/2. The repository's Pi sender has no control option, so
  this run used a small sender of my own (below).

Each decoded run missed only its edges. LeanDVB lost the first 6 to 10 data frames: it sat
misaligned on the dummy frames that run between the opening ID and the first message, then
found the first data frame and locked again (its log shows the re-detection). Its frame
counts show every data frame after that. The last 1,127 to 5,464 bytes of each raw stream never
went out, because they did not fill a frame. The S3 reports them as `input_pending`. The
documented behaviour is that raw streams leave a partial frame at EOF and the file container
pads it.

A first attempt at the 8PSK 2/3 run lost 1.28 s of samples in one B200 overflow, so its TS has
one gap of 8,570 packets in the same place; all 248,790 packets it did recover were exact. The
Mac stalled the B200's USB stream for about 1.25 s roughly once a minute, also in UHD's C++
`benchmark_rate` with no disk writes. Under `caffeinate -i -s -d` a 60 s benchmark and a 75 s
capture at 20 MS/s dropped nothing, and every later capture ran under it with no overflow.

## Setup

- CM5 (Raspberry Pi Compute Module 5, `/dev/spidev0.0`, READY on `/dev/gpiochip4` line 25)
  wired single-line to the XIAO's preset pins (`spipins xiao`), as in
  [SPI input](../../docs/spi-input.md#pins). It ran `examples/pi-sender/send.py` from this
  commit, copied with `examples/usb-send/send.py` and `host/filelink.py`, at `--hz 20000000`
  and `--mbps 20`, so READY or the sender's own speed set the rate.
- Transmitter: XIAO ESP32-S3 44:B1:76:B0:E5:8C, firmware 517c641
  ([self-check](../2026-10-23-ota-self-check/README.md)), callsign KJ5SCH, 2420 MHz, gain 0,
  peak 128. Coax with 40 dB of marked attenuation to the B200 (serial 34A6B8E), TX/RX port,
  receive only, gain 30 dB.
- Capture: continuous sc16 from the moment the keyed command was sent to 3 s after its end,
  10 MS/s for 5 Msym/s and 20 MS/s for 10 Msym/s. Every capture logs overflows and lost
  samples from the packet time stamps. Raw IQ stayed on the laptop.
- Receiver: LeanDVB, the build with the corrected short 3/4 table, with `host/decode.py`'s
  options, run over each whole capture.
- Sources, made on the Mac: three H.264 transport streams from ffmpeg's `testsrc2` (720p,
  25 fps, a running clock in the picture) with a 1 kHz AAC tone, constant bit rate with a fixed
  `-muxrate` of 9.678, 14.52 and 16.0 Mb/s, 40 s each. The file was `git archive` of 517c641
  (16,179,200 bytes). The VCM source was 40 MB of seeded random bytes. Hashes are in
  `runs.jsonl`.

## Method

1. Radio off: `spiinput on ts`, `dvbpace 13 5m 50`, then the sender. 36,769 messages,
   48,387,628 bytes, CRC chain 1623884184, all equal on both sides, zero errors. That checked the
   wire. Then `dvbpace 13 10m 45` with the sender unpaced: 72,595,824 bytes in 35.67 s, which is
   16.28 Mb/s, and READY never fell.
2. Keyed: start the capture, send `dvb 2420 MODE 70 0 128` (45 s for the file, 50 s for VCM),
   start the sender over ssh 7 s later, after the opening ID (4.98 s for KJ5SCH). Each source
   ends well before the closing ID.
3. Decode the whole capture. TS: align every received packet with the source file and count
   matches, gaps, backward jumps and packets found nowhere; then
   `examples/live-video/check_ts.py --reference` (continuity, PSI CRCs, exact slice). GCS:
   each frame's data field must be the next bytes of the source. File: `unpack --bbf --fec`
   and compare SHA-256. VCM: `host/decode.py`'s `live_vcm_match` against the bytes the sender
   confirmed and its control log.
4. Dummy frames inside the data stretch are the PLHEADERs with MODCOD 0 that LeanDVB logged
   between the first and the last frame that passed BCH.

## Runs

| Run | Mode | Input | Sender rate | Played / decoded | Exact | Gaps inside | Dummies inside | MER (dB) |
|---|---|---|---:|---:|---|---:|---:|---:|
| a | 8PSK 2/3, 5 Msym/s, 9.68 Mb/s | TS, a.ts | 9.66 Mb/s | 9,010 / 9,002 frames | 257,131 packets | 0 | 0 | 20.3 |
| b | QPSK 3/4, 10 Msym/s, 14.52 Mb/s | TS, b.ts | 14.50 Mb/s | 12,017 / 12,008 | 385,852 packets | 0 | 0 | 20.1 |
| c | QPSK 9/10, 10 Msym/s, 17.46 Mb/s | TS, c.ts, unpaced | 16.36 Mb/s | 11,012 / 11,004 | 425,174 packets | 0 | 7,178 | 19.7 |
| d | 8PSK 2/3, 5 Msym/s | GCS file, `--fec 16:4` | 9.50 Mb/s | 3,832 / 3,822 | 3,822 frames, SHA-256 equal | 0 | 0 | 19.9 |
| e | VCM 4, 13, 17, 11, 4 at 5 Msym/s | GCS, live controls | 8.23 Mb/s | 5,409 / 5,403 | 5,403 frames | 0 | 0 | 19.9 |
| a, first try | 8PSK 2/3, 5 Msym/s | TS, a.ts | 9.66 Mb/s | 9,010 / 8,711 | 248,790 packets | 1 (capture overflow) | 0 | 20.1 |

- Sender rate is payload bytes over the CM5's wall time for `send.py`, Python start included
  (the FEC run's includes the encoding). Mode rates are (Kbch - 80) bits per frame at the
  symbol rate, with pilots.
- Frames that passed BCH between the first and last good frame: 0 BCH failures in every run
  except the first try, which had 5 at its capture gap. LDPC corrected 0 to 32 bits in a run.
  MER is LeanDVB's PLHEADER MER, the median over that stretch.
- No run had a late bank fill. `check_ts.py` found 0 continuity errors, 0 duplicates and 0 PSI
  CRC errors in a, b and c. ffmpeg decoded 975 of the 1,000 pictures of each stream; the first
  second's group of pictures is lost with the first packets.
- In run e the controls left the CM5 at 0, 6, 12, 18 and 24 s. Each new MODCOD started at the
  first frame boundary after its control (`control_boundaries_valid`).

The VCM sender (run e) reads the source in 1,316-byte pieces and sends each as a GCS message
through `send.py`'s `message()`, with the same READY rule and 10 µs gap. When a control is due
it first sends a type 3 message carrying the MODCOD byte, using the next sequence number. The
S3 ran `vcm mixed` and `spiinput on gcs`; it counted 5 controls and rejected none.

## Records

`runs.jsonl`: the sources and their hashes; the radio-off records with the sender's stats; one
record per keyed run (board completion record, B200 settings and capture status, sender stats,
LeanDVB report, check); then the `check_ts.py` and `unpack` results.
