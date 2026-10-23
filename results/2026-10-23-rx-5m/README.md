# 12.64 Mb/s received on one ESP32-S3

One ESP32-S3 received short-frame 8PSK 8/9 DVB-S2 at 5 Msym/s from another and decoded all of
it on the chip: 12.64 Mb/s of payload. In three runs with the receiver started before the
transmitter keyed, it decoded 34,256, 34,249 and 34,220 consecutive frames over 38.3 s each,
every frame's CRC-32 equal to the transmitter's, none lost or failed. That is 9.1 times the
1.39 Mb/s of results/2026-10-22-rx-continuous on the same pair of boards.

Path: transmitter S3 → coax and 30 dB attenuation → receiver S3, 2420 MHz. Transmitter
d4957d3 (XIAO configuration), `dvb 2420 16 5m 55 0 128 --short --phy-solo`, keyed 2 s after
the receiver started. Receiver `esp_dvbs2_rx` b144c70 (`exp/rx-5m`), `rx 2420 32 45 16 5m`.
At this rate USB cannot carry the payload, so the receiver sends each frame's CRC-32 and
`host/rx.py` compares it with the CRC-32 of the model frame the frame's sequence implies.

| Run | Frames exact | Lost | CFO | Clock |
|---|---:|---:|---:|---:|
| 1 | 34,256 of 34,256 | 0 | -3,823 Hz | -1.36 ppm |
| 2 | 34,249 of 34,249 | 0 | -4,374 Hz | -1.55 ppm |
| 3 | 34,220 of 34,220 | 0 | -4,488 Hz | -1.56 ppm |

Core 1 (capture, unpack, matched filter at 3.2 samples per symbol, timing and carrier) used
on average 10.8 cycles per sample, about 132k of the 184,320 cycles in a 12,288-sample unit.
Core 0 derotates, demaps and descrambles (about 30% of the core) and checks each frame by
re-encoding (about 24%, 75k cycles per frame). No frame needed an LDPC iteration. During a
run about 18 KB of internal heap stayed free.

Before lock, while the receiver searched through noise and the transmitter's Morse ID, about
259 units (3.2M samples, 0.2 s) were dropped under overload in every run; the search is the
most expensive state at this rate. No data frame was affected.

A first run's check reported one wrong frame: two of the 53,700 model frames share a CRC-32
(birthday collision), so the lookup chose the wrong one. The checker now compares each frame
with the model frame its sequence implies. `runs.jsonl` holds each receiver summary and the
transmitter records.
