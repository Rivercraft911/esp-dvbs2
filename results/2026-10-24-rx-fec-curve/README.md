# The receiver's decoder near threshold, radio off

**Result.** The board's LDPC decoder, fed noisy frames exactly as the live receiver feeds it,
reaches a frame error rate (FER) of 1% about 1 dB above the DVB-S2 ideal threshold for QPSK 3/4
to 8/9 and 8PSK 8/9, and 1.5 to 3.5 dB above it for the lower code rates. In 72,000 frames over
all 12 short-frame modes it never returned a wrong payload. But on the live receiver it runs out
of time long before it runs out of code strength. With everything it touches in PSRAM, as
`firmware/rx/main/receive.c` places it, a frame with any bit error takes at least 7.4 ms to
decode, and the receiver hands it a frame only when it is idle. So the full receiver, replaying
noisy QPSK 3/4 captures, lost 43% of frames at 8 dB and none at 14 dB at 1 Msym/s, and 38% at
12 dB and none at 14 dB at 5 Msym/s, where the code alone decodes from about 5 dB. Two further
findings: the demodulator's two confidence levels per bit cost 0.5 dB at high code rates and
over 2 dB at rate 1/2, and a faster belief load, committed with this record, takes 0.5 to 1.2 ms
off every decoded frame without changing a single decoding.

## How the frames are made

`tests/rx_fec_bench` on the receiving S3 (44:B1:76:B0:E5:54), radio off, 240 MHz. Its
`curve MODCOD ESN0_CDB FRAMES` command builds each frame from a fresh GCS payload with the
transmitter's own encoder, maps it to QPSK or 8PSK as the receiver labels symbols (for 8PSK the
column interleaver as the release reads it, reversed at 3/5), adds complex Gaussian noise at the
given Es/N0, and demaps the symbols with the receiver's PIE kernels (`demod_qpsk_pie`,
`demod_8psk_pie`) into hard bits and weak bits (|soft| < 16, a point under half its nominal
distance from the boundary). The decoder gets no soft bits. Symbols carry no carrier, timing or
amplitude error: this measures the decoder, not synchronization. At 10 dB the hard bits' error
rate was 7.9e-4 against 7.7e-4 from Q(sqrt(Es/N0)) for Gray QPSK.

Each mode ran 200 frames per point from 0.5 dB below its Table 13 threshold to 4 dB above it in
0.5 dB steps (`python3 host/board.py "curve 7 503 200" --port PORT --timeout 900`, and so on),
three times: before the faster belief load (c3e0ba2) and after it (c0e0d6e), both with the
decoder's state in internal RAM and its work area in PSRAM, then with the state, work area and
frame copy all in PSRAM as on the live receiver (4379c96). The noise is seeded per frame, so all
three sweeps saw the same frames, and they decoded them identically: the same payloads, failures,
hard-bit errors and iteration histograms at all 120 points.

## Frame error rate

Table 13 of EN 302 307-1 gives the ideal Es/N0 for normal 64,800-bit frames at a packet error
rate of 1e-7. These are short frames at 1% FER, so the two criteria differ; the gap column is a
plain difference. Times are the live placement's and include the re-encode check.

| Mode | Table 13 | FER at most 1% from | Gap | Decode there, mean / p99 | 2 dB higher, mean |
|---|---:|---:|---:|---:|---:|
| QPSK 1/2 | 1.00 dB | 4.50 dB | +3.5 dB | 15.7 / 22.3 ms | 14.5 ms at 5.00 dB |
| QPSK 3/5 | 2.23 dB | 4.23 dB | +2.0 dB | 21.7 / 29.5 ms | 14.1 ms at 6.23 dB |
| QPSK 2/3 | 3.10 dB | 4.60 dB | +1.5 dB | 18.9 / 24.0 ms | 11.9 ms at 6.60 dB |
| QPSK 3/4 | 4.03 dB | 5.03 dB | +1.0 dB | 18.5 / 21.4 ms | 10.5 ms at 7.03 dB |
| QPSK 4/5 | 4.68 dB | 5.68 dB | +1.0 dB | 15.4 / 19.8 ms | 9.6 ms at 7.68 dB |
| QPSK 5/6 | 5.18 dB | 6.18 dB | +1.0 dB | 18.1 / 23.7 ms | 10.7 ms at 8.18 dB |
| QPSK 8/9 | 6.20 dB | 7.20 dB | +1.0 dB | 19.2 / 25.0 ms | 11.1 ms at 9.20 dB |
| 8PSK 3/5 | 5.50 dB | not reached (1.5% at 9.50 dB) | | 18.0 / 26.3 ms at 9.50 dB | |
| 8PSK 2/3 | 6.62 dB | 8.62 dB | +2.0 dB | 19.7 / 24.0 ms | 12.6 ms at 10.62 dB |
| 8PSK 3/4 | 7.91 dB | 9.41 dB | +1.5 dB | 17.2 / 21.4 ms | 10.7 ms at 11.41 dB |
| 8PSK 5/6 | 9.35 dB | 10.85 dB | +1.5 dB | 16.1 / 21.1 ms | 10.8 ms at 12.85 dB |
| 8PSK 8/9 | 10.69 dB | 11.69 dB | +1.0 dB | 19.9 / 28.1 ms | 11.6 ms at 13.69 dB |

The low code rates keep a tail: QPSK 1/2 still loses 2% of frames at 4.0 dB and 1% at 4.5 dB,
and 8PSK 3/5 1.5% at 9.5 dB. 8PSK 2/3 lost one frame of 200 at 10.62 dB after none at the three
points below.

## Where the dB go

On the host, the same three QPSK modes through three decoders on the same noise draws
(BPSK-equivalent per axis, soft values scaled as the demodulator scales them): float layered
belief propagation with full soft values and 50 iterations, `rx_fec.c` with int8 soft values
and 8 iterations, and `rx_fec.c` with the hard and weak bits alone, as on the board. First point
on a 0.25 dB grid where the FER is at most 1% (100 frames for float BP, 400 for the others):

| Mode | Float BP, 50 iterations | int8, soft, 8 | int8, two levels, 8 (the board) |
|---|---:|---:|---:|
| QPSK 1/2 | at most 1.00 dB (0 of 100 failed there) | 2.25 dB | 4.50 dB |
| QPSK 3/4 | 4.00 dB | 4.75 dB | 5.25 dB |
| QPSK 8/9 | 6.25 dB | 6.50 dB | 7.00 dB |

The host's two-level FER agrees with the board where they overlap (QPSK 3/4: 1.5% at 5.0 dB on
the host, 1.0% at 5.03 dB on the board; QPSK 1/2: 1.8% at 4.0 dB and 2.0%). So int8 arithmetic
with 8 iterations costs 0.25 dB at 8/9, 0.75 dB at 3/4 and at least 1.25 dB at 1/2, and the two
levels another 0.5 dB at 3/4 and 8/9 and over 2 dB at 1/2, with the tail. The receiver doc's
earlier figure of 0.3 to 0.5 dB for the two levels was an unretained estimate; it holds only at
the high rates. Soft values would also converge sooner near the waterfall: at QPSK 3/4 and
5.0 dB, 5.3 iterations on average against 6.8 for the frames each got right, at 8/9 and 7.0 dB
4.3 against 6.3; 2 dB above the 1% point both need about 3.

## Time per decoded frame

A decode is a fixed setup and then iterations, and no frame in any sweep needed fewer than 2.
`setup MODCOD` (4379c96) times the setup with a cold data cache: the check alone takes 20k to
45k cycles, and the check plus the belief load 406k (8/9) to 719k (1/2) cycles, 1.7 to 3.0 ms,
in the live placement. With only the work area in PSRAM it is 336k to 666k, all internal 244k to
444k; zeroing the packed messages in PSRAM alone takes 50k to 154k. A 2-iteration decode takes
7.4 to 9.4 ms in the live placement (QPSK 3/4: 7.9 to 8.0 ms, 3 iterations 9.2 to 10.2 ms). The
live placement is 1.12 to 2.17 times slower than the state in internal RAM, median 1.32: the
per-layer scratch and edge tables in `rx_fec_size` are read every layer.

## Can it keep up

The live receiver checks every frame and hands one that fails to the decoder only if the decoder
is idle (`receive.c`); otherwise the frame is lost. The decoder runs at the lowest priority on
core 0. So it can repair a damaged frame every ceil(t / T) frames at most, for decode time t and
frame time T (8.37 ms for QPSK short frames at 1 Msym/s, 1.67 ms at 5 Msym/s, 1.12 ms for 8PSK at
5 Msym/s), and less where the release and check leave it under t in each stretch: core 0 was 7
to 8% busy at 1 Msym/s, 39 to 43% at 5 Msym/s QPSK and 60 to 63% at 8PSK
([sweep](../2026-10-23-rx-sweep/README.md)). The rest of the frames must arrive with no bit
error at all.

- **1 Msym/s, QPSK.** At 3/4 a 2-iteration decode (7.9 to 8.0 ms) fits in a frame, a 3-iteration
  one (9.2 to 10.2 ms) does not, so the receiver repairs every frame only where nearly all damaged
  frames converge in 2 iterations, and at most every other one where they need 3.
- **5 Msym/s.** At 7.4 to 9.4 ms a decode, the decoder repairs at most about one QPSK frame in 8
  and one 8PSK frame in 17. For 88% of 16,200-bit frames to arrive error-free the hard bits' error
  rate must stay under 7.9e-6, about 12.7 dB for QPSK; for 94%, under 4.0e-6, about 18.2 dB for
  8PSK (Gray-mapped hard-decision error rates). These are predictions.

The full receiver app (4379c96) bore this out on noisy QPSK 3/4 captures from
`host/rx_twin.py`, uploaded and replayed with the radio off at the radio's pace
(`python3 host/rx.py 7 --rate 1m --replay FILE.sc8 --port PORT`). The twin adds the measured coax
link's carrier offset and drift, leak, image and DC, so these frames are harder than the bench's:

| Rate | Es/N0 | Frames | Good | Lost | Repaired by the decoder |
|---|---:|---:|---:|---:|---:|
| 1 Msym/s | 8 dB | 23 | 13 | 10 (43%) | 13, at most 3 iterations |
| 1 Msym/s | 10 dB | 24 | 22 | 2 (8%) | 22 |
| 1 Msym/s | 12 dB | 25 | 24 | 1 | 12 |
| 1 Msym/s | 14 dB | 25 | 25 | 0 | 1 |
| 5 Msym/s | 10 dB | 82 | 1 | 81, and 21 the demodulator dropped | 1 |
| 5 Msym/s | 12 dB | 129 | 80 | 49 (38%) | 15 |
| 5 Msym/s | 14 dB | 129 | 129 | 0 | 1 |
| 5 Msym/s | 16 dB | 129 | 129 | 0 | 0 |

Every lost frame failed with no iteration: it reached the decoder while the decoder was busy.
The live links in the records ran at a pilot MER of 25 to 30 dB, where nearly every frame passes
the check and the decoder idles: one frame of 5,725,408 needed it in the
[live demo](../2026-10-23-live-demo/README.md).

## The faster load (c0e0d6e)

The setup was dominated by building the beliefs: the old code called a per-bit function for all
16,200 bits from inside the IRAM frame path. Information groups now take four beliefs per word
from nibble tables, and parity groups keep the frame's pointers in registers (a char store may
alias them, so the compiler reloaded them every bit). With the decoder's state internal, the
check and load at QPSK 3/4 went from 689k to 453k cycles, and over the same 120 points and 24,000
frames each decoded frame took 0.49 to 1.15 ms less on average, median 0.95 ms (2 to 17%), with
every decoding unchanged. `tests/test_rx_fec.py` now covers this path: the Python model of the
decoder fed the same two levels matches it iteration for iteration on 27 frames at three parity
strides, before and after the change.

## What this does not show

The bench decodes on an otherwise idle core with only the noise generation in between; the live
receiver also runs the release and check on core 0. The bench's symbols have no synchronization
error; the twin's have the measured link's impairments, but a twin is still a model. 200 frames
per point resolve FER to about 0.5%, not an error floor, and each replay holds 23 to 129 frames.
Nothing here was keyed.

`runs.jsonl`: `curve` (one summary per point; `label` baseline, fast-load or live-placement,
with its `firmware` and `placement`; `iterations` is a histogram of 0 to 8 iterations and
`cycles_by_iterations` the mean cycles of each), `setup` (`setup MODCOD`, c0e0d6e with two
placements and 4379c96 with three), `replay` (`host/rx.py` summaries of the full receiver, with
the `rx_twin.py` command that makes each capture), `host_fer` and `host_iterations` (the host
comparisons, from `tests/test_rx_fec.py`'s compiled decoder, `float_bp` and `demodulate` with
k = 32, weak bits where |soft| < 16).
