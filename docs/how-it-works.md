# How it works

The ESP32-S3 makes every DVB-S2 frame and every output sample itself. One core encodes
payload; the other keeps two internal RAM banks ready for the radio's sample engine.
The design has two limits: enough encoder work per frame and enough fill work per bank.
[Board measurements](measurements.md) establish which configurations have met both.

## Encoder

The encoder follows [ETSI EN 302 307-1 V1.4.1](https://www.etsi.org/deliver/etsi_en/302300_302399/30230701/01.04.01_60/en_30230701v010401p.pdf).
Firmware uses normal frames, pilots enabled and physical-layer scrambling code zero.
A MODCOD number selects a modulation and error-correction rate (Table 12).

1. A baseband (BB) frame holds an 80-bit header and payload. The header records the input
   type and data length and includes a CRC-8 check. The frame is then scrambled (§§5.1, 5.2).
2. Bose-Chaudhuri-Hocquenghem (BCH) coding adds parity, followed by low-density parity-check
   (LDPC) coding. Every normal FEC frame is 64,800 bits (§5.3, Tables 5a–7a, Annex B).
3. QPSK maps two bits to one of four phases; 8PSK maps three to one of eight. An interleaver
   changes bit order for 8PSK (§§5.3.3, 5.4). QPSK phases are the odd multiples of π/4;
   8PSK phases are multiples of π/4.
4. Physical-layer (PL) framing adds a 90-symbol header, pilot blocks and a known phase
   rotation sequence (§5.5). A normal frame with pilots has 33,282 QPSK symbols or
   22,194 8PSK symbols.

`host/dvbs2.py` is the independent bit-level reference. `dvbs2.c` is a portable C encoder;
`fast.c` uses packed bits and tables, with S3 PIE assembly for the hot stages. PIE is the
S3's 128-bit vector instruction extension. Every C stage is compared against Python,
including transport-stream (TS) packet continuation and generic continuous stream (GCS)
input. Standard worked examples and independently produced stage digests anchor the reference.

BCH is polynomial division over bits: shift the remainder, XOR the next input bit,
then XOR the generator polynomial when the outgoing bit is one (§5.3.1). Tables combine
several such steps without changing the remainder. LDPC uses the standard's sparse parity
addresses in groups of 360 bits; whole-word operations and vector XORs replace bit loops.
Packing, mapping and framing retain the exact standard phase sequence.

During synthetic operation, each frame's test bytes come from xorshift32 seeded with 2026.
The first 40 bytes carry the stored callsign. SPI can instead supply TS or GCS input;
its protocol is documented separately in [spi-input.md](spi-input.md).

## Fill

The DAC takes signed 10-bit in-phase (I) and quadrature (Q) values. Each complex sample is
one 32-bit word: I in bits 9:0 and Q in bits 19:10. Pulse shaping smooths transitions
between symbols with a root-raised-cosine filter (§5.6 and Annex A).

For N samples per symbol, symbol x[k] and pulse h in DAC least-significant-bit (LSB) units:

```
y[k*N+p] = sum(a=0..L-1, x[k-a] * h[a*N+p])       DAC LSB
```

The ordinary pulse spans L=9 symbols. A table for every complete history would be too large,
so it is split into three groups of three symbols. Three table words are added for each
sample. The 8PSK 20m pulse spans six symbols and uses two groups. Table offsets encode
the symbol history, so the runtime does table reads and additions rather than filter math.
Integer taps have units of 1/256 DAC LSB; constellation coordinates use 2^-30 units.
Each group is rounded after division by 2^38. Nonnegative per-axis biases total 512;
XOR 0x80200 converts the final packed sum to signed values without cross-axis carries.

The pulse coefficients include inverse sinc correction for the DAC's sample hold
(Annex A). A digital dump has no analog hold, so its measured signal quality differs
from a model including one. Neither model establishes the radio's response.

At 80 MS/s, QPSK uses four-symbol blocks, three tables and 256 possible windows per table.
The storage is `3 * 256 * 4 * N * 4 B = 12288*N B`: 49,152 B at N=4 and
36,864 B at N=3. The global symbol timeline continues through frame boundaries.

Fast modes keep compressed frames in internal RAM: two phases per byte for 8PSK and
slower QPSK, four phases per byte for QPSK at 80 MS/s. Core 0 publishes completed slots;
core 1 releases them only after consuming their last required symbols. Finished overlap
samples are cached, so both banks receive identical copies without doing the shaping twice.
If the frame ring starves, a valid DVB-S2 dummy frame preserves framing, but carries no
payload. Any underrun fails the normal regression check.

## Bank geometry

The DAC test engine loops a 16,380-sample bank until the CPU switches it. Two physical
64 KiB banks at 0x3fcc0000 and 0x3fcd0000 are reserved by the linker. Four trailing
physical words in each bank are unused. Only aligned word/vector stores write samples.
The undocumented sample format and playback mechanism were established by the
[credited radio projects](prior-art.md).

The engine retains its sample offset at a bank switch. Sample n lives at word
`n % 16380` in either bank. A bank includes a lead before its scheduled start and a tail
after its scheduled end, so either bank supplies the same samples near a switch.
The constants come from `radio_s3.h` and the [archived geometry measurements](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-16-20msym/README.md).

```
lead + step + tail = 16380                         samples
period = step / Fs                                seconds
deadline = step * Fcpu / Fs                        CPU cycles
```

| Fs | Lead | Tail | New samples (step) | Period | Deadline, Fcpu=240 MHz |
|---|---:|---:|---:|---:|---:|
| 40 MS/s | 64 | 2,048 | 14,268 | 356.7 µs | 85,608 cycles |
| 80 MS/s | 128 | 512 | 15,740 | 196.75 µs | 47,220 cycles |

For example, `15740 samples / (80e6 samples/s) * (240e6 cycles/s) = 47220 cycles`.
The switch aims half a lead early. Normal interrupt levels are masked during the short
wait and switch. The fill runs with interrupts enabled and counts their cost.
A fill using the entire period leaves zero compute margin; overlap cannot make up
for a continuously late producer. Actual DAC start latency and RF sample continuity
still need an external capture.

The radio starts through the vendor PHY's continuous-wave setup, stops that test tone,
retunes the PLL, restores the intended gain and then plays bank samples. Automatic filter
selection chooses the wide mode for wide signals and records readbacks. A register
readback is evidence of configuration, not measured analog bandwidth.

## Sign windows and 40 MHz

QPSK's I and Q are each a sequence of positive or negative values. At 100/3 Msym/s,
the 80 MS/s DAC produces 12 samples for every five symbols. A seven-symbol pulse at
roll-off 0.20 gives nominal bandwidth `(1+0.20)*(100/3) MHz = 40 MHz`.

Each four-sample vector depends on eight signs per axis. Complete rounded I and Q
values can therefore be looked up independently in small 256-row tables and combined
with OR. For the three vectors in one twelve-sample group, the chronological windows
are [-6..1], [-5..2] and [-3..4]. Table construction sums one whole axis before rounding.
No addition of independently rounded pulse fragments is needed in the renderer.

The encoder writes eleven-sign histories directly into queue pages, including headers
and pilots. The renderer selects the appropriate eight-sign window for each vector.
There is no intermediate packed frame ring for this mode. A full queue blocks the
encoder until the renderer releases eight pages. History survives every frame and bank seam.

The 65,536 B shaping store holds `24576 + 54*528 + 4096 + 8144 = 65328 B`:
axis tables, 54 guarded queue pages, a sign lookup table and the encoder workspace.
Each page represents 128 five-symbol groups, or 1,536 samples. Full coverage is
`54*1536 / 80e6 s = 1.0368 ms`. Internal input and overlap buffers are separate allocations.
Acquire/release counters transfer ownership between cores; physical page cursors remain
valid through 32-bit counter wrap.

This mode met keyed bank deadlines, but the encoder averaged 237,370 of 239,630 cycles
per frame. Queue coverage absorbed slower frames. It has not met the proposed 15% reserve,
and has no demonstrated live-input or RF decode at this rate. [Evidence](measurements.md#40-mhz).

## Rates

| Rate token | Modulation | DAC | Samples/symbol | Pulse span |
|---|---|---:|---:|---:|
| omitted | QPSK | 40 MS/s | 40 | 9 symbols |
| `5m` | QPSK, 8PSK | 40 MS/s | 8 | 9 symbols |
| `10m` | QPSK, 8PSK | 40 MS/s | 4 | 9 symbols |
| `20m` | QPSK | 80 MS/s | 4 | 9 symbols |
| `20m` | 8PSK | 80 MS/s | 4 | 6 symbols |
| `26m667` | QPSK | 80 MS/s | 3 | 9 symbols |
| `33m333` | QPSK | 80 MS/s | 12/5 | 7 symbols |

QPSK MODCODs 4..11 select rates 1/2, 3/5, 2/3, 3/4, 4/5, 5/6, 8/9, 9/10.
8PSK MODCODs 12..17 select 3/5, 2/3, 3/4, 5/6, 8/9, 9/10. Fractional names are
exact ratios: 80/3 and 100/3 Msym/s. Integer telemetry truncates those rates.

## Checking changes

The compact CI suite tests the Python reference against standard vectors and independent
stage digests, compares every portable C encoder stage with Python, and runs the actual
stream lifecycle with host task stubs. All pinned dump CRCs must remain exact. The
fractional stream is also checked sample by sample against independent integer convolution.
Allocation failures, repeated commands, frame seams, bank wraps and counter wrap are covered.
Host clocks and heaps are synthetic; only [the board example](../examples/self-check/README.md)
can check deadlines and startup memory. RF decoding and spectral measurements remain separate work.

```sh
python3 -m pip install numpy scipy pyserial
(. ~/.espressif/tools/activate_idf_v6.1.sh >/dev/null 2>&1 && cd firmware && idf.py build)
python3 tests/test_dvbs2.py
python3 tests/test_dvbs2_8psk.py
python3 tests/test_dvbs2_c.py
python3 tests/test_stream.py
python3 tests/test_rational.py
python3 tests/test_self_check.py
python3 examples/self-check/check.py --plan
```

CI runs these, the SPI checks, and a firmware build. If a sandbox blocks ESP-IDF's unused
component manager, use `IDF_COMPONENT_MANAGER=0 idf.py build`; no managed dependencies are needed.
