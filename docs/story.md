# How it got here

The transmitter fits on the S3 by turning pulse shaping into table reads and keeping
its working data in internal RAM. The [implementation guide](how-it-works.md) gives the
encoder and sample formats; [measurements](measurements.md) links the recorded runs.

## Two budgets

Core 0 builds complete DVB-S2 frames: headers, BCH, LDPC, modulation and pilots. Core 1
shapes symbols into the radio's two sample banks. At 80 MS/s, a new bank is due every
196.75 µs, or 47,220 cycles at 240 MHz. Both cores must keep up; buffering only absorbs
short variations in work per frame.

Samples stay in internal RAM. The radio consumes 320 MB/s at 80 MS/s; PSRAM is useful
for payload storage, but the live sample path cannot depend on its bandwidth or cache.
The linker reserves the radio banks and checks placement of the shaping store and kernels.

## Tables instead of filter math

As in the [RP2350 I/Q benchmark](https://github.com/Rivercraft911/RP2350_IQ_Benchmark/tree/88ab018e65a51805ff562842177a4d47b1b04b2e),
pulse shaping looks up precomputed filter outputs. Ordinary modes split a nine-symbol
pulse into smaller windows and add their table words. PIE vector instructions load and
store several samples at once. Packed frame storage and a cache of overlapping samples
reduce both memory traffic and repeated work.

For 40 MHz QPSK, I and Q are independent sign streams. Core 0 queues compact I/Q bit
planes; core 1 extracts sign windows and combines complete axis values while rendering.
Two frames are queued before playback. One notification per bank wakes the encoder after
space becomes available.

## What has been shown

| Check | Evidence | Limit |
|---|---|---|
| Encoder and shaping | Standard vectors, independent stage digests, C/Python comparisons and waveform CRCs | Host tests do not establish board timing |
| S3 output samples | USB dumps decoded bit-exact in LeanDVB | Before RF |
| Bank deadlines | Timed radio-off and contained keyed runs | Timing does not establish RF continuity or quality |
| 40 MHz reserve | 40fa63d: 150,667 mean encoder cycles/frame, 39,448 maximum fill cycles/bank | Prepared payload, 30 s keyed; [records](measurements.md#40-mhz) |
| SPI input | Single-line and quad on-chip loopback | External sender throughput remains unmeasured |
| Video | Host-model MPEG-2 TS → IQ → LeanDVB → TS | No S3, external camera or RF in the dry run |

With a prepared payload, the 40 MHz keyed tests left 37% mean encoder headroom against
239,630 cycles/frame and 16% fill headroom against 47,220 cycles/bank. Keeping Wi-Fi
running or tearing it down gave encoder means of 150,667 and 150,669 cycles/frame.
The matched radio-off mean was 152,754, with additional profiling enabled.

The historical e96795b run used 198,879 encoder cycles/frame and 39,548 fill cycles/bank,
meeting the 15% reserve while generating fresh xorshift payload bytes for every frame.
Its roughly 47k-cycle gap from radio-off pacing came from different payload sources.
The matched-source tests show no measurable encoder penalty from keying.
A wider receiver is needed to observe the complete signal; the E200 on the bench
covers at most 20 MHz.

Synthetic test payloads carry a callsign header. External SPI/USB payload bytes receive
no added header. The transmitter sends separate 20 WPM Morse ID before and after a run;
see [station identification](transmitting.md#station-identification).

Start with the [host checks](how-it-works.md#checking-changes),
[radio-off S3 self-check](../examples/self-check/README.md) or
[host video dry run](../examples/live-video/README.md#rf-free-dry-run).
Contained RF decoding, spectrum and live-camera input still need measurement.
Additional raw reports are preserved in the
[archive tag](https://github.com/Rivercraft911/esp-dvbs2/tree/archive/dev-2026-10-17).
