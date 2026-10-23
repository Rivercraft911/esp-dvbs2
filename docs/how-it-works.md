# How it works

The ESP32-S3 makes every DVB-S2 frame and every output sample itself. One core encodes
payload; the other keeps two internal RAM banks ready for the radio's sample engine.
The design has two limits: enough encoder work per frame and enough fill work per bank.
[Board measurements](measurements.md) establish which configurations have met both.

## Encoder

The encoder follows [ETSI EN 302 307-1 V1.4.1](https://www.etsi.org/deliver/etsi_en/302300_302399/30230701/01.04.01_60/en_30230701v010401p.pdf).
Firmware uses normal frames by default, pilots enabled and physical-layer scrambling code zero.
QPSK MODCODs 4..10 (1 or 5 Msym/s) and 8PSK MODCODs 12..16 (5 Msym/s) can instead use short frames
(`--short`): 16,200-bit FEC frames, BCH with t = 12 over GF(2^14) (168 parity bits), 8,370 QPSK or
5,598 8PSK symbols with pilots (Tables 5b, 6b, 7b, Annex C). The 8PSK bit interleaver has 5,400 rows.
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

`host/dvbs2.py` and `host/dvbs2_tables.py` are importable reference libraries, not command-line
tools. `dvbs2.c` is a portable C encoder;
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
selection restores PBUS(4,2)=32 for wide signals and 0 for narrow signals after CW,
then records fresh readbacks. A register readback is evidence of configuration,
not measured analog bandwidth.

## Sign windows and 40 MHz

QPSK's I and Q are each a sequence of positive or negative values. At 100/3 Msym/s,
the 80 MS/s DAC produces 12 samples for every five symbols. A seven-symbol pulse at
roll-off 0.20 gives nominal bandwidth `(1+0.20)*(100/3) MHz = 40 MHz`.

Each four-sample vector depends on eight signs per axis. Complete rounded I and Q
values can therefore be looked up independently in small 256-row tables and combined
with OR. For the three vectors in one twelve-sample group, the chronological windows
are [-6..1], [-5..2] and [-3..4]. Table construction sums one whole axis before rounding.
No addition of independently rounded pulse fragments is needed in the renderer.

The encoder queues compact I/Q bit planes, including headers and pilots: forty words
per page, each holding sixteen signs per axis. Before playback starts, the queue holds two
complete frames. Core 1 extracts the three eight-sign windows directly from rolling I/Q
bit streams while rendering. A fixed 80-symbol schedule combines table loads with
packed I/Q addition and stores; the disjoint DAC fields make addition exactly equal
to OR. There is no intermediate expanded history page.
There is no intermediate packed frame ring for this mode. A full queue blocks the
encoder until the renderer releases at least eight pages. One notification at bank end
wakes it after both callbacks finish. History survives every frame and bank seam.

The 65,536 B shaping store holds `24576 + 105*160 + 768 + 8144 = 50288 B`:
axis tables, 105 queue pages, a 192-sample scratch chunk and the encoder workspace.
The headline radio-off pace also borrows its 7,264-byte reference input: 57,552 B
total, without additional heap. At a physical bank cut, only a partial chunk uses
scratch and a word copy; full chunks render directly to the bank. The last sixteen
signs carry across pages. Linker assertions keep the live kernel, cut wrapper and
copy kernel in IRAM, and the complete shaping store in internal DRAM.
Each page represents 128 five-symbol groups, or 1,536 samples. Full coverage is
`105*1536 / 80e6 s = 2.016 ms`. The overlap buffer remains a separate internal allocation.
Acquire/release counters transfer ownership between cores; physical page cursors remain
valid through 32-bit counter wrap.

Firmware e96795b met the 15% reserve on both cores in one 30 s keyed run: encoder mean
198,879 of 239,630 cycles/frame, maximum fill 39,548 of 47,220 cycles/bank.
With the later wide-filter fix, a single B200 capture recovered 40 exact frames at this rate
from a prepared, repeated payload. Sustained external input at this rate remains unqualified.
[Timing](measurements.md#40-mhz), [RF capture](../results/2026-10-20-wide-filter/README.md).

## 8PSK at 36 MHz

At 80/3 Msym/s, 8PSK uses a five-symbol pulse with roll-off 0.35:
`(1+0.35)*(80/3) MHz = 36 MHz`. Core 0 publishes compact four-symbol descriptors;
core 1 renders them directly into the DAC banks. All three measured code rates,
2/3, 5/6 and 9/10, ran at full rate in [30 s keyed tests](measurements.md#8psk-at-36-mhz).

The encoder keeps BCH, PRBS, Gold and mapper tables in internal RAM to avoid
competing with the renderer for flash cache. Its largest table allocation comes
first, before the context and 8,144-byte FEC workspace, so smaller allocations
cannot fragment that block. Cold cache tails, extra BCH data, dummy descriptors
and prepared input use PSRAM. These allocations exist only for 8PSK `26m667`.
The vector renderer stays in IRAM. Its small control path is flash-backed and
locked in instruction cache only while this mode is active, occupying at most
one 4 KiB way of the configured 16 KiB cache. Setup repeats after Wi-Fi starts;
cleanup unlocks it. Other modes retain their own memory layouts.

The shared 65,536-byte shaping store holds the complete pulse table, queue and
stream mux, with no extra queue allocation:

```
36864 table bytes + 54 * 528 page bytes + 160 mux bytes = 65536 bytes
```

Each page contains 128 four-symbol descriptors, a prefetch guard and metadata,
representing up to 1,536 samples. A normal 22,194-symbol frame needs 44 pages.
Ten spare pages let mapping bursts overlap playback; mean encoder time alone
was insufficient to choose the depth. After Wi-Fi and worker allocations exist,
startup samples free internal heap and its command low-water mark. It selects
all 54 resident pages only with at least 20,000 B minimum free and 22,048 B
currently free (including a 2,048 B transient guard); otherwise it aborts before
playback. It then prefills the queue without allocating from Wi-Fi's heap.
Radio-off dumps use 54/50/46 pages at MODCOD 13/15/17 without Wi-Fi.

Payload, BB, BCH and LDPC for the next frame run before waiting for pages; only
mapping and PL publication block. Scratch cannot be reused until the mapped
frame is fully published. Acquire/release counters transfer page ownership;
the producer arms its wait before rechecking space, and the consumer notifies
it once per completed bank. Physical cursors survive 32-bit counter wrap.

The fill never waits. At each PLFRAME boundary it admits a complete frame, or
a partial frame covered by a measured production bound. Let G be the largest
recurring page-publication gap after bulk mapping, excluding full-queue sleep,
W = 2G cycles/page, P the ready pages, and R = 9 reserve pages. Wake allowance
D is zero during mapping or 47,220 cycles while awaiting a bank-end wake.
Every missing page k = 1 through 44-P must satisfy:

```
D + k*W <= (P-R+k-1)*2944                         CPU cycles
```

The renderer needs at least `128*23 = 2944` single-issue instructions per page.
The linear inequality needs only its endpoints checked; thresholds update after
EOF publication. This bound depends on measured production and wake latency;
it cannot cover arbitrary CPU starvation. Records expose bound misses and dummies.
If admission fails, a standard 3,330-symbol DVB-S2 dummy PLFRAME preserves framing
(EN 302 307-1 §5.5.1). Dummy pairs retain four-symbol rounding and vector alignment;
boundary history is spliced into the same renderer. No data frame or input byte
is skipped. Startup and steady dummies are counted separately.

The [self-check](../examples/self-check/README.md) checks a golden dump and all
three paced code rates. Profiling adds bounded work and can insert steady dummies;
those paces do not establish the full-rate keyed result. Digital dumps decode
bit-exact. Single B200 captures now recover exact prepared payloads at all three code rates;
live input throughput at this rate remains unqualified.
[RF records](../results/2026-10-20-wide-filter/README.md).

## Receiving

A second S3 decodes short-frame QPSK at 1 Msym/s on its own (`firmware/rx`, with the
`dvbs2rx` component). Core 1 turns radio samples into frames of soft bits; core 0 checks or
decodes each frame and sends the payload over USB.

```
core 1: dump banks, 16 MS/s -> int8 I/Q -> matched filter at each symbol time
        -> carrier, leak and DC removal -> PL descrambling -> LLRs and hard bits -> frame ring
core 0: re-encode check -> (if needed) layered min-sum LDPC -> BCH and BBHEADER -> USB
```

**Capture.** The radio's dump engine writes 10-bit I/Q into a 16,384-word ring in whichever of
three 64 KiB SRAM banks it owns. A timer interrupt moves ownership to the next bank every
12,288 samples (768 µs, 184,320 cycles): it wakes 15 µs early, spins to the exact index and
re-anchors, so handoffs land within a few samples. The task then processes released banks and
may trail by a unit. If it trails by two, the interrupt takes back the newest bank it has not
started; the loss reaches the demodulator as a jump in the sample index.

Wi-Fi starts only to power and calibrate the PHY. The run holds the PHY through the
Bluetooth modem bit, as `physolo` does for the transmitter, keeps the Wi-Fi clock domain
referenced and deinitializes Wi-Fi. Stopping Wi-Fi also clears bit 1 of
`SYSCON_WIFI_BB_CFG_REG`, which the dump needs: without it the samples hold a constant.
The receiver sets it again; a diff of the radio registers with Wi-Fi up and stopped found it.
No Wi-Fi interrupt or periodic work runs during a capture. Run memory is allocated after
the teardown, and both task stacks are static in internal RAM.

**Front end.** Each 10-bit word becomes int8 I and Q, floor(s10/4), at 1.55 cycles per
sample. A 96-tap root-raised-cosine matched filter is evaluated only at symbol times, to 1/64
symbol, from 64 coefficient sets of seven 16-byte vectors. On PIE that costs about 61 cycles
per symbol including scheduling.

**Synchronization.** Acquisition correlates the signs of matched-filter outputs against the
start-of-frame and PLS code. It locks on the next PLHEADER one frame later, at the clock the
carrier offset implies: both radios run from 40 MHz crystals, so the symbol clock offset equals
the carrier offset divided by the carrier frequency, held to 0.01 ppm in every capture. Each
header and pilot block then updates the timing, carrier, DC and leak estimates. The data
between two blocks is derotated with a phase interpolated between them. Removing the
transmitter's carrier leak, about -17 dB in its own frame, raises MER from about 16 dB to
about 29 dB.

**FEC.** A clean frame is checked by re-encoding its hard bits with the transmitter's fast BCH
and LDPC and comparing parity: 31.6k cycles. Other frames go to a layered normalized min-sum
decoder in int8 that uses the code's 360-column structure, 16 checks per PIE vector. It runs
393k cycles per iteration with its 48 KB work area in PSRAM, for at most 8 iterations with an
early stop.

| Measured, 1 Msym/s QPSK 3/4 short | Cycles |
|---|---:|
| Capture service per 12,288-sample unit (edges, bank clear) | about 25k |
| Demodulator per unit, mean / worst (lock) | about 85k / 124k |
| FEC per 8.37 ms frame on core 0, mean | about 103k |

With Wi-Fi gone, about 101 KB of internal RAM is free before a run; the demodulator takes
25 KB, the FEC 18 KB and the frame ring 8 KB, leaving about 49 KB.
[Results](../results/2026-10-22-rx-continuous/README.md),
[demodulator measurements](../results/2026-10-22-rx-demod/README.md).

## Rates

| Rate token | Modulation | DAC | Samples/symbol | Pulse span |
|---|---|---:|---:|---:|
| omitted | QPSK | 40 MS/s | 40 | 9 symbols |
| `5m` | QPSK, 8PSK | 40 MS/s | 8 | 9 symbols |
| `10m` | QPSK, 8PSK | 40 MS/s | 4 | 9 symbols |
| `20m` | QPSK | 80 MS/s | 4 | 9 symbols |
| `20m` | 8PSK | 80 MS/s | 4 | 6 symbols |
| `26m667` | QPSK | 80 MS/s | 3 | 9 symbols |
| `26m667` | 8PSK | 80 MS/s | 3 | 5 symbols |
| `33m333` | QPSK | 80 MS/s | 12/5 | 7 symbols |

QPSK MODCODs 4..11 select rates 1/2, 3/5, 2/3, 3/4, 4/5, 5/6, 8/9, 9/10.
8PSK MODCODs 12..17 select 3/5, 2/3, 3/4, 5/6, 8/9, 9/10. Fractional names are
exact ratios: 80/3 and 100/3 Msym/s. Integer telemetry truncates those rates.

## Table provenance

Current waveform coverage is `tests/test_stream.py`; `tests/test_rational.py` also checks
fractional samples against independent convolution. To recover the original generators
without changing this checkout, extract the archive into a new directory:

```sh
mkdir -p notes
TABLE_SRC=$(mktemp -d "$PWD/notes/table-source.XXXXXX")
git archive archive/dev-2026-10-17 | tar -x -C "$TABLE_SRC"
```

Run these with the host Python. Outputs stay inside that extracted directory.

| Table or oracle | Exact invocation |
|---|---|
| DVB-S2 tables | `python3 "$TABLE_SRC/host/gen_tables.py" /path/to/en_30230701v010401p.pdf` |
| Ordinary pulse taps | `python3 "$TABLE_SRC/host/shaper.py" header` |
| QPSK pulse taps | `python3 "$TABLE_SRC/host/qpsk_fill.py" --header` |
| Six-symbol pulse derivation and check | `python3 "$TABLE_SRC/tests/test_shaper_short.py"` |

The ETSI generator checks PDF SHA-256
`19077f80420eb17519fcf6f83e2847d7a877940b134c5306814b8c6f60d8475d` and requires
Poppler's `pdftotext`. Keep retained fixtures unchanged unless waveform behavior changes.

## Checking changes

The compact CI suite tests the Python reference against standard vectors and independent
stage digests, compares every portable C encoder stage with Python, and runs the actual
stream lifecycle with host task stubs. All pinned dump CRCs must remain exact. The
fractional stream is also checked sample by sample against independent integer convolution.
Allocation failures, post-Wi-Fi heap admission, dummy transitions, repeated commands, frame seams,
bank wraps and counter wrap are covered.
Host clocks and heaps are synthetic; only [the board example](../examples/self-check/README.md)
can check deadlines and startup memory. RF decoding and spectral measurements remain separate work.

```sh
python3 -m pip install numpy scipy pyserial
for test in tests/test_*.py; do python3 "$test" || exit 1; done
(. ~/.espressif/tools/activate_idf_v6.1.sh && cd firmware && idf.py build)
python3 examples/self-check/check.py --plan
python3 examples/pi-sender/send.py --vector
python3 examples/self-check/build_receiver.py
python3 examples/live-video/dry_run.py notes/video-dry --receiver "$PWD/notes/leansdr/src/apps/leandvb"
```

Use the [host shell and IDF setup](../README.md#quick-start); no board is needed for these
commands. CI runs all host test scripts and a firmware build. The receiver build and
video dry run are separate integration checks. If a sandbox blocks ESP-IDF's unused
component manager, use `IDF_COMPONENT_MANAGER=0 idf.py build`; no managed dependencies are needed.
