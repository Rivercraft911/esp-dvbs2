# 12.64 Mb/s with margin: no dropped units, 30 dB MER

The ESP32-S3 receiver decoded short-frame 8PSK 8/9 DVB-S2 at 5 Msym/s in three 38.3 s runs,
34,232, 34,199 and 34,196 consecutive frames, every frame's CRC-32 equal to the transmitter's,
with no capture unit dropped at any point. results/2026-10-23-rx-5m (same boards, b144c70)
dropped about 259 units in every run while searching before lock. The median pilot MER rose
from about 24 dB to 29.9 dB, and core 1 now uses 121k of the 184,320 cycles in a unit.

Path and settings as in results/2026-10-23-rx-5m: transmitter S3 → coax and 30 dB attenuation →
receiver S3, 2420 MHz. Transmitter d4957d3, `dvb 2420 MC 5m 55 0 128 --short --phy-solo`
(1 Msym/s: `dvb 2420 7 55 0 64 --short --phy-solo`), keyed 2 s after the receiver started.
Receiver `esp_dvbs2_rx` e379dfb (`exp/rx-5m`), `rx 2420 32 45 MC 5m`, checked by `host/rx.py`
against the transmitter's model (CRC-32 at 5 Msym/s, payload bytes at 1 Msym/s).

| Run | Frames exact | Dropped units | Pilot MER, lowest / median | CFO | Clock |
|---|---:|---:|---|---:|---:|
| 8PSK 8/9, 5 Msym/s, 12.64 Mb/s | 34,232 of 34,232 | 0 | 27.1 / 29.9 dB | -2,379 Hz | -0.67 ppm |
| 8PSK 8/9, 5 Msym/s, 12.64 Mb/s | 34,199 of 34,199 | 0 | 27.0 / 29.9 dB | -4,113 Hz | -1.42 ppm |
| 8PSK 8/9, 5 Msym/s, 12.64 Mb/s | 34,196 of 34,196 | 0 | 26.8 / 29.9 dB | -4,571 Hz | -1.66 ppm |
| QPSK 3/4, 5 Msym/s, 6.95 Mb/s | 22,892 of 22,892 | 0 | 27.7 / 29.8 dB | -5,349 Hz | -2.14 ppm |
| QPSK 3/4, 1 Msym/s, 1.39 Mb/s | 4,576 of 4,576 | 0 | 28.2 / 30.4 dB | -6,521 Hz | -2.73 ppm |

No frame failed the re-encode check, none was lost, the demodulator never relocked, and the
worst capture handoff came 10 samples late with at most one unit of lag.

## What changed since b144c70

- **Matched filter.** A 3-tap symbol-spaced equalizer fitted to the real 5 Msym/s symbols found
  -0.034 to -0.037 at ±1 symbol, real and symmetric, stable to 0.001 across frames and three
  captures (1 Msym/s: -0.004): the analog filters broaden a 6.75 MHz-wide signal. The 5 Msym/s
  coefficient table now cancels 0.035 at no cost per symbol. Data MER on five recorded 5 Msym/s
  dumps went from 23.2-24.3 dB to 27.2-29.1 dB.
- **Search.** At 5 Msym/s candidates come every 1.6 samples, five times as many per sample as at
  1 Msym/s. The half sums are now made 320 at a time from candidates aligned so that every 8
  samples hold five of them, without divisions; the search covers 4,096 of every 16,384 samples
  to bound its cost. Its prefilter now asks for the 8 newest differential signs instead of 3:
  the transmitter's Morse ID is a tone that gives one sign throughout, and the newest three
  header terms of MODCOD 16 are equal, so every candidate used to pass. Radio-off replays,
  cycles per 12,288-sample unit: TX-off noise, mean 59k and worst 110k (was 106k and 199k);
  keyed tone then frames, worst 166k at the lock (was 247k during the tone).
- **Memory.** Core 0 task stacks 4,096 → 2,560 bytes and replay as a capture source: 8.8 KB less
  static RAM.

## Cost

Core 1 per 12,288-sample unit (capture task: unpack, matched filter, known blocks, search),
from the receiver's own counters:

| Run | Mean | Worst push | Worst unit with capture service |
|---|---:|---:|---:|
| 8PSK 8/9, 5 Msym/s | 120.8k (9.8 cycles/sample) | 141.6k to 164.5k | 171.5k to 224.6k, at the lock |
| QPSK 3/4, 5 Msym/s | 117.2k | 158.0k | 168.9k |
| QPSK 3/4, 1 Msym/s | 79.7k | 104.8k | 131.9k |

The worst unit is the one that locks (search, timing refine and the first tracking in one
unit); capture absorbs it with one unit of lag. Core 0 releases the queued symbol runs into hard
and weak bits (30% of the core at 8PSK 8/9, 94k cycles per frame) and checks each frame by
re-encoding (24%, 76k cycles per frame, wall clock with the release task preempting it).

Internal heap during runs: 34.2 KB free after setup (largest block 15 KB), at least 24.8 KB
throughout. Stack high-water marks left free: release 1,452, check 1,676, decode 1,900 (no frame
needed decoding), capture 2,264 bytes.

`runs.jsonl` holds each receiver summary followed by its transmitter record.
