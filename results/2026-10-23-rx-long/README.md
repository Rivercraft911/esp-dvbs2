# The S3 receiver over 540 s

**Result.** The S3-to-S3 link ran three 540 s qualification runs with every frame exact and
none lost:

| Mode | Frames exact | Lost |
|---|---:|---:|
| QPSK 3/4 short, 1 Msym/s, 1.39 Mb/s | 62,539 of 62,539 | 0 |
| 8PSK 8/9 short, 5 Msym/s, 12.64 Mb/s | 467,539 of 467,539 | 0 |
| 8PSK 8/9, TS input replayed | 467,527 of 467,527 | 0 |

- The TS run handed on 4,399,229 packets. None had a CRC-8 error, a pattern error or a jump.
- No frame failed, needed the LDPC decoder or was dropped.
- The demodulator never relocked, and no capture sample was dropped.
- The heap counter stayed at 0 allocations after boot.

The about-1 s bursts of dropped frames seen in some earlier runs did not occur. The receiver now
reports its slowest USB record write, so a burst can be attributed. Here the slowest was 312 us,
and no write took 1 ms.

## Setup

- **Boards.** Transmitter 44:B1:76:B0:E5:54 (XIAO) and receiver 44:B1:76:B0:DA:C8, both on
  a1ded15. That is main ee0fd01 plus this branch's receiver changes; the transmitter code is
  main's.
- **Link.** Coax and 30 dB attenuation, 2420 MHz, short frames with pilots.
- **Receiver.** `rx 2420 32 530 MC [5m] [ts pattern]`, through `host/rx.py --seconds 530`.
- **Transmitter.** `dvb 2420 MC [5m] 540 0 PEAK --short`, keyed 2 s after the receiver
  started: 540 s is the transmitter's limit. Peak was 64 at 1 Msym/s and 128 at 5 Msym/s.
  Its input was `spiinput off` (the synthetic xorshift32 payload), or `spiinput quad ts replay`
  for the TS run.
- **Window.** The receiver stops before the closing ID. It saw 523.4 s of data: frames
  from model frame 1 or 2 on.
- **Method.** As [ts-link](../2026-10-23-ts-link/README.md) and
  [rx-5m-margin](../2026-10-23-rx-5m-margin/README.md).

## Results

| Mode | Frames exact | Missing | Failed | Pilot MER lowest / median | Slowest write | Writes over 1 ms | Transmitter |
|---|---:|---:|---:|---|---:|---:|---|
| QPSK 3/4, 1 Msym/s | 62,539 of 62,539 | 0 | 0 | 27.2 / 30.2 dB | 88 us | 0 | 0 underruns, 0 late fills |
| 8PSK 8/9, 5 Msym/s | 467,539 of 467,539 | 0 | 0 | 26.2 / 29.7 dB | 292 us | 0 | 0, 0 |
| 8PSK 8/9, 5 Msym/s, TS | 467,527 of 467,527 | 0 | 0 | 24.9 / 29.7 dB | 312 us | 0 | 0, 0 |

**TS.** The receiver handed on 4,399,229 packets, all it should have from those frames:

- No CRC-8 errors, pattern errors, jumps, resyncs or unchecked packets.
- 78,557 continuity errors, exactly the replay loop's wraps (56 packets, not a multiple of 16).

**MER per minute.** Lowest and median of the pilot MER, from `mer_minutes`:

| Minute | QPSK 3/4 1 Msym/s | 8PSK 8/9 5 Msym/s | 8PSK 8/9 TS |
|---:|---|---|---|
| 0 | 28.0 / 30.2 | 26.2 / 29.7 | 24.9 / 29.7 |
| 1 | 27.2 / 30.2 | 26.8 / 29.8 | 27.0 / 29.7 |
| 2 | 28.0 / 30.2 | 26.8 / 29.7 | 27.0 / 29.7 |
| 3 | 28.1 / 30.2 | 26.9 / 29.7 | 26.7 / 29.7 |
| 4 | 28.4 / 30.2 | 27.2 / 29.7 | 26.6 / 29.7 |
| 5 | 28.1 / 30.2 | 26.3 / 29.7 | 26.8 / 29.7 |
| 6 | 28.3 / 30.3 | 26.6 / 29.7 | 26.9 / 29.7 |
| 7 | 27.7 / 30.2 | 26.8 / 29.7 | 26.6 / 29.6 |
| 8 | 27.7 / 30.2 | 26.6 / 29.7 | 26.0 / 29.7 |

The median moved by at most 0.1 dB over nine minutes. The carrier offset at the end was
-7.8 kHz, -4.3 kHz and -4.1 kHz, and the clock offset -3,151, -1,601 and -1,396 ppb.

## Load

These are wall-clock cycles from the receiver's counters at 240 MHz. Core-0 counters include
preemption by the release task, so they are upper bounds and overlap.

| | QPSK 3/4, 1 Msym/s | 8PSK 8/9, 5 Msym/s | 8PSK 8/9, TS |
|---|---:|---:|---:|
| Core 1, mean per 12,288-sample unit (184,320 cycles) | 82.6k | 132.2k | 132.5k |
| Core 1, slowest unit | 122.2k | 156.4k | 174.6k |
| Frame period | 2,008,800 | 268,704 | 268,704 |
| Core 0 release per frame | 110.5k (5.5%) | 95.3k (35.5%) | 95.4k (35.5%) |
| Core 0 check per frame | 47.1k (2.3%) | 76.0k (28.3%) | 76.7k (28.6%) |
| Core 0 TS deframer per frame | | | 56.7k (21.1%), 6.0k per packet |
| Core 0 pattern check per frame (test only) | | | 31.2k (11.6%) |

**Core 1's mean is higher than earlier records.** The 5 Msym/s mean is 132k, against about 121k
in [ts-link](../2026-10-23-ts-link/README.md) and
[rx-no-wifi](../2026-10-23-rx-no-wifi/README.md). This most likely reflects the share of units
spent locked to a signal: about 99% of these 530 s, against about 84% in a 45 s run. A 20 s
check run on the same boards averaged 109k. That explanation is derived, not measured per
unit. Units slower than one unit period were absorbed by capture lag, as before: the largest
service time was 211k. No sample was dropped.

**The TS deframer costs twice what ts-link measured.** It now takes 56.7k cycles a frame (6.0k
per packet), against 25.6k (2.72k per packet) on c3a1c2d. The likely cause is 8dd02c7, which
came after that record. It keeps a fingerprint of each PID's last payload packet to check
duplicates. It was not measured separately.

At 8PSK 8/9 with the test pattern check, the core-0 counters add up to 260k of the 268.7k frame
period. That sum is an upper bound, because it counts preemption twice. No frame was dropped
in 540 s, but the margin is thin. Without the pattern check, which the egress does not need,
the sum is 229k (85%).

**Memory.** Every run had:

- `heap_after_boot` at 0 allocations and 0 frees.
- 49,914 B internal free after the receiver's allocations, with the largest block 30,720 B.
- 16,268 B internal minimum since boot.
- Stack minimum free per task (bytes): release 1,444, check 1,668, decode 1,892.

## Changes behind these runs (a1ded15)

**Host checker.** `host/rx.py` could not check a 540 s run. Its model covered 60 s, and at
5 Msym/s it would have built 850 MB of payloads one byte at a time. Now:

- The xorshift32 model comes from jumps per frame row (GF(2) byte tables) and numpy. 480,000
  frames of CRC keys take 3.5 s.
- The TS replay keys repeat with the 56-packet loop (frame 0 aside).
- The model covers the run plus a minute.
- The summary adds `mer_minutes` and `missing_runs` (first sequence, length, seconds into the
  run).
- `tests/test_rx_host.py` checks the fast stream against xorshift32 stepped byte by byte, the
  CRC keys against the payloads, and the periodic TS keys against `ts_model` over two loops.

**Receiver.** The receiver's summary adds three fields:

- `max_write_us`: the slowest frame record written to USB. The write blocks while the host does
  not read.
- `max_write_sequence`: that frame's sequence number.
- `slow_writes`: records that took 1 ms or more.

A burst of dropped frames next to a slow write points at USB. The cost is two timer reads per
frame. The 5 Msym/s record CRC is now computed before the USB lock is taken, not inside it.

## Records

`runs.jsonl` holds, per run, the receiver's summary (role `rx`) and the transmitter's records
(role `tx`). The last rows, marked `set: check-20s`, are the 20 s 8PSK 8/9 check run, which had
12,027 of 12,027 frames exact.
