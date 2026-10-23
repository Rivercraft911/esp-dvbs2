# First continuous on-chip receive

One ESP32-S3 transmitted short-frame QPSK 3/4 DVB-S2 at 1 Msym/s and 2420 MHz. A second
ESP32-S3 received it through 30 dB of coax and decoded everything itself: 16 MS/s capture,
matched filter, timing, carrier, LDPC, BCH and BBHEADER checks. The host only compared the
payload bytes the receiver sent over USB with the transmitter's xorshift model.

In run 1 the receiver decoded 30.0 s of capture: 3,583 consecutive frames, every one
byte exact, 5,209,682 payload bytes at 1.390 Mb/s. No frame failed or went missing.
Some frames needed up to five LDPC iterations.

Receiver `esp_dvbs2_rx` 312a7cc (`exp/rx`), command `rx 2420 32 30 7`. Transmitter 73473df
(main, XIAO configuration), `dvb 2420 7 45 0 64 --short`. Both boards are XIAO-class S3s.

| Run | Receiver start | Result |
|---|---|---|
| 1 | 7 s after keying | pass: 3,583 of 3,583 frames exact over 30 s |
| 2, 3 | 7 s after keying | fault after 3 units, during acquisition |
| 4 | 10 s after keying | fault after 3 units, during acquisition |
| 5 | 10 s after keying | 3,541 of 3,541 frames exact, then a fault at 29.65 s when the closing ID began |

Every fault was the same: acquisition took about 238k cycles per 12,288-sample unit
(184k available) on noise or on the Morse ID tone, three units in a row, and the bank
handoff ran out of slack. Tracked frames never faulted.

During run 1 the demodulator on core 1 averaged 7.76 cycles per sample; 15 are available.
Bank service is separate. FEC on core 0 averaged 103k cycles per 8.37 ms frame (5%); the
slowest frame, with five iterations and the decoder's work area in PSRAM, took 2.94M.
The internal heap low-water mark was 7,344 bytes. The latest bank handoff was 13 samples.

This is one frequency, one receiver gain (index 32), one board pair and a conducted path.
The receiver had to start inside the transmitter's data interval. `runs.jsonl` holds each
run's receiver and transmitter records and the payload check.
