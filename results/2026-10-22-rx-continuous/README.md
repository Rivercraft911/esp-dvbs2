# Continuous receive on one ESP32-S3

One ESP32-S3 received a continuous DVB-S2 stream from another and decoded all of it on the
chip: short-frame QPSK 3/4 at 1 Msym/s, 1.390 Mb/s of payload. In three qualification runs
the receiver started listening 2 s before the transmitter keyed, locked on the first data
frame after the Morse ID and decoded 4,581, 4,578 and 4,577 frames, every one byte exact,
over 38.3 s each. No frame failed or went missing, and no capture unit was dropped.

Path: transmitter S3 (XIAO class) → coax and 30 dB attenuation → receiver S3 (XIAO class),
2420 MHz. Receiver `esp_dvbs2_rx` 8ced9dd (`exp/rx`), `rx 2420 32 45 7`. Transmitter 73473df
(main, XIAO configuration), `dvb 2420 7 55 0 64 --short`, keyed 2 s after the receiver
started. The host only compares the payload bytes the receiver streams over USB with the
transmitter's xorshift model (`host/rx.py`).

| Run | Frames exact | Span | Worst push per unit | Worst service | FEC mean / max |
|---|---:|---:|---:|---:|---:|
| 1 | 4,581 of 4,581 | 38.34 s | 123,502 cycles | 164,032 | 102.8k / 1.57M |
| 2 | 4,578 of 4,578 | 38.32 s | 123,481 | 163,651 | 104.3k / 1.55M |
| 3 | 4,577 of 4,577 | 38.31 s | 113,949 | 140,577 | 104.3k / 1.55M |

A 12,288-sample unit lasts 184,320 cycles at 240 MHz. The demodulator on core 1 averaged
6.92 cycles per sample. "Service" adds the bank edge search and sentinel clear. FEC ran on
core 0 and averaged about 103k cycles per 2.01M-cycle frame (5%). The slowest frames took 2
LDPC iterations with the decoder's work area in PSRAM. The latest bank handoff was 16 samples.
The internal heap never fell below 6,128 bytes, and the largest free block after setup was
5,632 bytes.

Before these runs, nine stability runs used the same code under an earlier build label.
Seven of them decoded 34,433 frames, of which 2 failed FEC, one each in the first two runs.
Both were isolated frames with about 30 dB pilot MER and 14 or fewer failing parity checks
on arrival, between perfect neighbours. Their cause is not known yet; the receiver now
streams the soft bits of any failed frame for diagnosis. The other two produced no frames:
the host checker crashed in the first, so the board was still busy for the second.

Limits: one frequency, one receiver gain (index 32), one pair of boards and a conducted path.
Wi-Fi stays initialized because the dump engine stops without it. Internal RAM is nearly
full. `runs.jsonl` holds every receiver summary and the transmitter records.
