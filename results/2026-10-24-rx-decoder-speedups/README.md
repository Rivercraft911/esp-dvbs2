# Three decoder fixes: internal scratch, four soft levels, a queue that keeps the TS in order

**Result.** Three changes to the receiver, each found by the
[decoder curve](../2026-10-24-rx-fec-curve/README.md), let it repair frames it used to lose. The
full receiver app, replaying noisy QPSK 3/4 captures with the radio off, now keeps every frame at
8 dB at 1 Msym/s (13 of 23 before) and every frame at 13 dB at 5 Msym/s. With a transport stream
the repaired frames now go back into the stream in order: at 9 dB all 24 frames needed the
decoder and all 24 reached the stream, every packet checked on the chip, where before a repaired
frame never reached the stream at all. No decoding changed except by the four levels, and no
frame came back with a wrong payload.

1. **The decoder's per-layer scratch in internal RAM** (f4aa8bf). Every layer reads and writes it;
   in PSRAM a decode took 1.1 to 2.2 times as long. `rx_fec_set_scratch` moves it to a 13.6 KB
   block the receiver takes from the heap once at boot.
2. **Four soft levels at 1 Msym/s** (47de36d, dce4bbb). The demapper adds a third bit plane,
   `fine`, set where 8 <= |s| < 16 or |s| >= 32, so with the weak bit each code bit has one of four
   confidence levels, which the decoder turns into the beliefs its soft-value path would build
   from 4, 12, 24 or 48. Bit-exact with that path on the host (31 decoded and 14 failed frames).
   The fine plane doubles the demapper's cost, so the receiver uses it only at 1 Msym/s.
3. **Three queued decodes and an in-order stream** (0f08122). A frame that fails its check waits
   in one of three PSRAM jobs instead of being lost when the decoder is busy, and in TS mode a
   portable module (`dvbs2rx/rx_order.c`) holds up to 16 frames behind a pending decode so a
   repaired frame enters the stream in sequence.

The 8PSK four-level kernel first kept two locals in its caller's register save area and
crashed the board with a double exception on its first run; the fix gives it its own frame
(dce4bbb). Moving both four-level kernels to flash (they run only
at 1 Msym/s) brought the static DRAM margin under the capture banks back from 16 to 800 bytes.

## The decoder, radio off

`tests/rx_fec_bench` `curve`, 200 frames a point, the same seeded noise as the
[decoder curve](../2026-10-24-rx-fec-curve/README.md), on 44:B1:76:B0:E5:54. Moving the scratch
(bench f4aa8bf) left every decoding unchanged:

| Point | All in PSRAM | Scratch internal | Faster |
|---|---:|---:|---:|
| QPSK 1/2, 4.50 dB | 15.73 ms | 14.48 ms | 1.09x |
| QPSK 3/4, 5.03 dB | 18.47 ms | 14.59 ms | 1.27x |
| QPSK 3/4, 7.03 dB | 10.47 ms | 8.39 ms | 1.25x |
| 8PSK 8/9, 11.69 dB | 19.92 ms | 10.33 ms | 1.93x |
| 8PSK 8/9, 13.69 dB | 11.60 ms | 6.64 ms | 1.75x |

Mean time per decoded frame, check included. Four levels against two on the same frames (bench
5e60297, scratch internal):

| Mode, Es/N0 | FER, two levels | FER, four levels | Mean decode, two / four |
|---|---:|---:|---:|
| QPSK 1/2, 2.50 dB | 31.0% | 1.0% | 22.5 / 21.0 ms |
| QPSK 1/2, 3.00 dB | 14.5% | 1.5% | 20.3 / 18.8 ms |
| QPSK 1/2, 3.50 dB | 4.0% | 0.5% | 17.7 / 16.9 ms |
| QPSK 3/4, 4.53 dB | 87.0% | 5.0% | 16.1 / 17.6 ms |
| QPSK 3/4, 4.78 dB | 16.5% | 0% | 16.3 / 15.7 ms |
| QPSK 3/4, 5.03 dB | 1.0% | 0% | 14.6 / 14.2 ms |
| QPSK 8/9, 6.50 dB | 89.5% | 9.5% | 12.1 / 14.0 ms |
| QPSK 8/9, 6.75 dB | 24.5% | 0% | 13.1 / 12.1 ms |
| QPSK 8/9, 7.00 dB | 0% | 0% | 11.6 / 10.7 ms |
| 8PSK 3/4, 8.66 dB | 92.5% | 16.5% | 16.0 / 18.0 ms |
| 8PSK 3/4, 8.91 dB | 33.5% | 1.5% | 16.5 / 16.6 ms |
| 8PSK 8/9, 11.19 dB | 35.0% | 1.5% | 13.1 / 13.1 ms |
| 8PSK 8/9, 11.44 dB | 1.0% | 0% | 12.1 / 11.4 ms |

About 0.25 dB at QPSK 3/4 and 8/9 and 8PSK 8/9, and 1 to 2 dB at QPSK 1/2, where two levels had
a long tail. The host had predicted the same (four levels within about 0.25 dB of int8 soft
values at 3/4 and 8/9). The decode takes about as long as before: at the edge of the waterfall
it now finishes frames it used to give up on, above it it needs fewer iterations.

## The four-level kernels

`tests/rx_demod_bench` (dce4bbb) runs each PIE kernel against its C version on random symbols,
saturation corners and odd tails, every plane compared byte for byte: all equal. Cycles for 1,440
symbols, the old kernels beside the new:

| Kernel | C, four levels | PIE, two levels | PIE, four levels |
|---|---:|---:|---:|
| QPSK | 157,719 | 11,384 | 20,937 |
| 8PSK | 357,865 | 17,495 | 36,416 |

That is about 54k more cycles a QPSK short frame and 71k a 8PSK one in the release on core 0:
2.7% of a frame at 1 Msym/s, but 13 and 26% of core 0 at 5 Msym/s, where the decoder's time is
the limit, so the receiver keeps two levels there.

## The full receiver, replaying noisy captures

`python3 host/rx.py 7 --rate 1m --replay FILE.sc8 --port PORT` (add `--ts replay` for TS) on
captures from `host/rx_twin.py`, which carries the measured coax link's carrier offset and
drift, leak, image and DC. QPSK 3/4; frames good of frames found:

| Rate | Es/N0 | Before (rx-fec-curve) | Scratch internal (f4aa8bf) | All three (bff2413) |
|---|---:|---:|---:|---:|
| 1 Msym/s | 6 dB | | | 18 of 23 |
| 1 Msym/s | 7 dB | | | 21 of 23 |
| 1 Msym/s | 8 dB | 13 of 23 | 21 of 23 | 23 of 23 |
| 1 Msym/s | 10 dB | 22 of 24 | 24 of 24 | 24 of 24 |
| 5 Msym/s | 12 dB | 80 of 129 | 84 of 129 | 90 of 129 |
| 5 Msym/s | 13 dB | | 126 of 129 | 129 of 129 |

With a transport stream (`rx_twin.py --ts`, the transmitter's replay pattern checked on the chip
packet by packet), all three fixes:

| Es/N0 | Frames good | Repaired | Into the stream in order | Packets checked | Wrong | Jumps |
|---:|---:|---:|---:|---:|---:|---:|
| 7 dB | 21 of 23 | 21 | 21 | 159 | 0 | 2 |
| 9 dB | 24 of 24 | 24 | 24 | 185 | 0 | 0 |

The two jumps at 7 dB are the two frames the decoder could not repair. The ordering module held
at most 4 frames at once, and never had to give one up.

## Memory

After boot the receiver now keeps 15.0 KB of internal heap free (35.7 KB before): 13.6 KB went to
the scratch and 6.1 KB to the three slots' fine planes, as two blocks because the heap's largest
free block (15.4 KB) cannot hold both together. The first build with one block could not run
any mode (`arena_fits` false) and the boot check said so. The PSRAM arena grew from 96 to 144 KB
for the three jobs (18 KB with their fine planes) and the 16-frame ordering buffer (32 KB). Static
DRAM ends 800 bytes below the capture banks.

## What this does not show

Nothing was keyed. The replays hold 23 to 129 frames each. The four-level kernels' cost was
measured on the bench, not inside a live run at 1 Msym/s. The ordering buffer's size (16 frames,
17.9 ms at 5 Msym/s 8PSK) was not stressed: no replay filled it.

`runs.jsonl`: `curve` (`label` scratch internal or levels, with `levels` 2 or 4 in each record),
`demod_bench` (one full pass of `tests/rx_demod_bench`), `replay` (`host/rx.py` summaries with the
`rx_twin.py` command that makes each capture; `label` scratch internal or all three).
