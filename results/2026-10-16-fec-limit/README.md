# Fast encoder on the board

The fast encoder (`firmware/components/dvbs2/fast.c` and the PIE kernels in `pie.S`) runs
bit-exact on the ESP32-S3 and encodes an 8PSK 2/3 normal frame in at most 214k cycles with GCS
input, against 2.35 M for the plain encoder: 20% of one core at 5 Msym/s. The 8PSK video mode
no longer needs a faster encoder. In the live transmit stream it brings QPSK 1/2 encoding from
36% to 13% of core 0, and the stream's output is bit-identical to the plain encoder's.

## What ran

Firmware `445aa21` (main with exp/fec-limit merged), ESP32-S3 at 240 MHz, radio off.

- `fecbench`, `fecbench c` and `fecbench pie`: 14 MODCODs × GCS and TS input × 5 frames each,
  one warm-up frame, encoder pinned to one core (record fields `core`, `payload_memory`,
  `output_memory` say where the buffers were). `python3 tests/test_fecbench.py runs.jsonl`
  checks every output CRC against the Python reference and prints the tables below.
- `dvbdump 4 40`, `dvbdump 11 40`: the continuous stream (results/2026-10-16-stream) now encoded
  by the fast path, decoded by LeanDVB with `host/onair.py --stream 40` (`decodes.jsonl`).
- `dvbpace 4 60`, `dvbpace 11 60`: the bank schedule against the cycle counter for 60 s with
  the fast encoder on core 0, no hardware register access.

## Correctness

- All 140 output CRCs equal the reference on each of the three paths (plain, portable C, PIE).
- The PIE self-check compared every vector stage with the C stage on one frame per MODCOD:
  14 records, 0 mismatches, `pie_mask` 15 (all four vector stages active).
- `ldxq_bias` 0: the silicon computes `EE.LDXQ.32` addresses as the TRM states. QEMU had
  differed by 4 bytes; the firmware measures this at start-up, so both were handled.
- The QPSK 1/2 stream dump has CRC 3046131439, the same as the plain-encoder dump of the same
  40 frames: the fast path changes no output byte. Both stream dumps decode bit for bit (38 of
  40 frames, from frame 0, 0 LDPC bit flips).

## Cycles per frame, PIE path

Median and maximum over 5 frames; share of one 240 MHz core at the given symbol rate.

| Mode | GCS median / max | TS median / max | Max share at 5 Msym/s (GCS / TS) |
|---|---:|---:|---:|
| 8PSK 3/5 | 238k / 238k | 272k / 273k | 22% / 26% |
| 8PSK 2/3 | 213k / 214k | 251k / 260k | 20% / 24% |
| 8PSK 3/4 | 286k / 287k | 334k / 335k | 27% / 31% |
| 8PSK 5/6 | 313k / 313k | 363k / 367k | 29% / 34% |
| 8PSK 8/9 | 202k / 202k | 250k / 252k | 19% / 24% |
| 8PSK 9/10 | 203k / 204k | 253k / 255k | 19% / 24% |
| QPSK 1/2 | 701k / 715k | 731k / 748k | 9% at 1 Msym/s |
| QPSK 9/10 | 739k / 750k | 789k / 799k | 9% at 1 Msym/s |

For comparison, the portable C path takes at most 1.02 M cycles per 8PSK 2/3 frame (96% at
5 Msym/s) and the plain encoder 2.35 M. QPSK frames are larger (33,282 symbols) and the
benchmark writes their output to PSRAM (`output_memory`), which is why they cost more per frame.

The agent's own target, every PIE 8PSK 2/3 frame under 250k cycles, is met with GCS input
(214k) and missed with TS input (260k): the TS packet CRC-8 adds about 38-46k cycles. The model
predicted 179k (GCS) and 205k (TS) warm; the board is 19% and 22% above it.

What the encoder alone allows on one core (GCS, maximum cycles): 8PSK 2/3 up to about
24.9 Msym/s, 8PSK 9/10 26.1, 8PSK 5/6 17.0. At 20 Msym/s, 8PSK 2/3 would take 80% of a core
and 5/6 would not fit; at 10 Msym/s, 5/6 takes 59%. These leave out the stream's own work
(below) and payload input.

## In the live stream

| Run | Fills | Late | Underruns | Encoder mean / max per frame | Share of core 0 | Min free internal |
|---|---:|---:|---:|---:|---:|---:|
| `dvbpace 4 60` | 168,209 | 0 | 0 | 1.045 M / 1.102 M | 13% | 58,351 B |
| `dvbpace 11 60` | 168,209 | 0 | 0 | 1.125 M / 1.181 M | 14% | 57,487 B |

Before the fast path the same runs took 2.88 M (QPSK 1/2) per frame, 36%. The stream's frame
cost is about 340k cycles above `fecbench pie` for QPSK 1/2: it also generates the payload,
writes the frame to PSRAM and shifts every phase code to the shaper's format in a separate
pass. Folding that shift into the mapper is the next obvious saving. The longest bank fill was
41,672 cycles and the smallest slack 43,670 of 85,608.

Free internal heap during a run is 57-58 KB, down from about 76 KB, because the per-mode fast
context (about 15 KB) lives in internal RAM; the workspace moved to PSRAM. Wi-Fi start-up's
own need has still not been measured; `dvb` reports `min_free_internal` and stops before
keying if an allocation fails.

## Not shown

- No RF. DAC reads of bank memory and Wi-Fi traffic on core 0 were not running.
- 8PSK is not yet wired into `dvb` (the 8 and 4 samples/symbol shaper kernels exist).
- Encoder timing with payload arriving over SPI.
