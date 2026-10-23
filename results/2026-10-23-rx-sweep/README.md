# Receiver sweep: every short-frame mode the transmitter sends, every frame exact

The ESP32-S3 receiver decoded all 19 short-frame modes the S3 transmitter can send:
- QPSK 1/2 to 8/9 at 1 Msym/s, 0.83 to 1.69 Mb/s.
- QPSK 1/2 to 8/9 at 5 Msym/s, 4.15 to 8.45 Mb/s.
- 8PSK 3/5 to 8/9 at 5 Msym/s, 8.46 to 12.64 Mb/s.

Each run was 30 s. Every frame from lock to the end was there and exact: 223,153 frames in
all. No frame failed its re-encode check or needed the LDPC decoder, no frame or capture unit
was dropped, and the receiver never relocked.

Path: transmitter S3 (44:B1:76:B0:E5:54) → coax and 30 dB attenuation → receiver S3
(44:B1:76:B0:DA:C8), 2420 MHz. Both boards ran c3a1c2d (`info` records at the top of
`runs.jsonl`). The transmitter sent its synthetic GCS payload (`spiinput off`):
`dvb 2420 MC [5m] 40 0 PEAK --short --phy-solo`, peak 64 at 1 Msym/s and 128 at 5 Msym/s, keyed
2 s after the receiver started. The receiver ran `rx 2420 32 30 MC [5m]`, checked by
`host/rx.py`. It stops before the transmitter's data ends. After the opening Morse ID, each
run checks about 23.5 s of frames.

| Mode | MODCOD | Msym/s | Payload, Mb/s | Frames exact | Lost | Failed | Decoded / max iterations | Pilot MER, lowest / median, dB | Core 1 per unit, mean / worst | Core 0, release + check | Free RAM, bytes | Dropped / relocks | Result |
|---|---:|---:|---:|---:|---:|---:|---:|---|---|---|---:|---|---|
| QPSK 1/2 | 4 | 5 | 4.15 | 14,076 of 14,076 | 0 | 0 | 0 / 0 | 27.3 / 29.8 | 112.4k / 168.7k | 26% + 13% | 36,447 | 0 / 0 | exact |
| QPSK 3/5 | 5 | 5 | 5.66 | 14,001 of 14,001 | 0 | 0 | 0 / 0 | 26.6 / 29.8 | 112.3k / 169.1k | 26% + 15% | 36,063 | 0 / 0 | exact |
| QPSK 2/3 | 6 | 5 | 6.30 | 14,040 of 14,040 | 0 | 0 | 0 / 0 | 27.7 / 29.8 | 112.5k / 218.1k | 26% + 15% | 35,631 | 0 / 0 | exact |
| QPSK 3/4 | 7 | 5 | 6.95 | 14,035 of 14,035 | 0 | 0 | 0 / 0 | 26.8 / 29.7 | 112.6k / 160.5k | 26% + 16% | 35,215 | 0 / 0 | exact |
| QPSK 4/5 | 8 | 5 | 7.38 | 14,019 of 14,019 | 0 | 0 | 0 / 0 | 23.7 / 29.7 | 112.6k / 202.0k | 26% + 16% | 34,927 | 0 / 0 | exact |
| QPSK 5/6 | 9 | 5 | 7.81 | 14,040 of 14,040 | 0 | 0 | 0 / 0 | 27.5 / 29.7 | 112.6k / 218.4k | 26% + 17% | 34,655 | 0 / 0 | exact |
| QPSK 8/9 | 10 | 5 | 8.45 | 14,002 of 14,002 | 0 | 0 | 0 / 0 | 22.2 / 29.7 | 112.5k / 207.4k | 26% + 17% | 34,223 | 0 / 0 | exact |
| 8PSK 3/5 | 12 | 5 | 8.46 | 20,975 of 20,975 | 0 | 0 | 0 / 0 | 22.7 / 29.6 | 116.2k / 177.7k | 35% + 25% | 36,063 | 0 / 0 | exact |
| 8PSK 2/3 | 13 | 5 | 9.42 | 20,971 of 20,971 | 0 | 0 | 0 / 0 | 24.2 / 29.6 | 116.1k / 226.1k | 35% + 26% | 35,631 | 0 / 0 | exact |
| 8PSK 3/4 | 14 | 5 | 10.39 | 21,042 of 21,042 | 0 | 0 | 0 / 0 | 25.5 / 29.7 | 116.5k / 221.7k | 35% + 26% | 35,215 | 0 / 0 | exact |
| 8PSK 5/6 | 15 | 5 | 11.68 | 21,188 of 21,188 | 0 | 0 | 0 / 0 | 19.2 / 29.6 | 116.7k / 177.5k | 35% + 27% | 34,655 | 0 / 0 | exact |
| 8PSK 8/9 | 16 | 5 | 12.64 | 21,059 of 21,059 | 0 | 0 | 0 / 0 | 27.2 / 29.8 | 116.5k / 177.8k | 35% + 28% | 34,223 | 0 / 0 | exact |
| QPSK 1/2 | 4 | 1 | 0.83 | 2,824 of 2,824 | 0 | 0 | 0 / 0 | 20.1 / 30.3 | 78.4k / 133.8k | 5% + 2% | 36,447 | 0 / 0 | exact |
| QPSK 3/5 | 5 | 1 | 1.13 | 2,810 of 2,810 | 0 | 0 | 0 / 0 | 22.2 / 30.3 | 78.2k / 132.0k | 5% + 2% | 36,063 | 0 / 0 | exact |
| QPSK 2/3 | 6 | 1 | 1.26 | 2,810 of 2,810 | 0 | 0 | 0 / 0 | 27.8 / 30.2 | 78.3k / 133.5k | 5% + 2% | 35,631 | 0 / 0 | exact |
| QPSK 3/4 | 7 | 1 | 1.39 | 2,817 of 2,817 | 0 | 0 | 0 / 0 | 28.6 / 30.2 | 78.3k / 132.8k | 5% + 2% | 35,215 | 0 / 0 | exact |
| QPSK 4/5 | 8 | 1 | 1.48 | 2,826 of 2,826 | 0 | 0 | 0 / 0 | 23.6 / 30.3 | 78.4k / 133.1k | 5% + 2% | 34,927 | 0 / 0 | exact |
| QPSK 5/6 | 9 | 1 | 1.56 | 2,811 of 2,811 | 0 | 0 | 0 / 0 | 28.4 / 30.2 | 78.3k / 131.3k | 5% + 2% | 34,655 | 0 / 0 | exact |
| QPSK 8/9 | 10 | 1 | 1.69 | 2,807 of 2,807 | 0 | 0 | 0 / 0 | 26.1 / 30.3 | 78.3k / 131.4k | 5% + 3% | 34,223 | 0 / 0 | exact |
| 8PSK 3/5 to 8/9 | 12..16 | 1 | | | | | | | | | | | not run: the transmitter sends 8PSK short frames only at 5 Msym/s |

**Columns.**
- *Lost*: sequence numbers missing between the first and last frame.
- *Failed*: frames that failed both the re-encode check and the decoder.
- *Decoded*: frames that needed the LDPC decoder, and the most iterations any used.
- *Pilot MER*: per frame, from the header and pilot blocks. The lowest frame of a run ranges
  from 19.2 to 28.6 dB; medians are 29.6 to 30.3 dB.
- *Core 1*: cycles per 12,288-sample capture unit (768 us, 184,320 cycles at 240 MHz). The
  worst unit includes capture service. Units over 184,320 cycles were absorbed by capture lag:
  at most one unit behind, none dropped.
- *Core 0*: wall-clock cycles of the release and check tasks per frame, as a share of the frame
  period. Each includes the other's preemption, so the sum is an upper bound.
- *Free RAM*: internal heap after the receiver's allocations. The re-encode check's tables grow
  with the code rate.

The heap's lowest point since boot (`min_free`) was 11,316 bytes in every run, reached while
Wi-Fi powers up the PHY, before the receiver allocates anything. Earlier results show 6.1 to
25.0 KB there.
