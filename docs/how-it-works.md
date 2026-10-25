# How it works

Two ESP32-S3s, two apps, no PC in the signal path. `firmware/tx` turns payload into DVB-S2
samples and plays them through the S3's own radio, up to 69.8 Mb/s in 36 MHz. `firmware/rx`
captures the other S3's signal through its own radio and decodes every frame on the chip, up
to 12.64 Mb/s. This page is the overview and the shared ground rules; the two halves have
their own pages:

- [The transmitter](transmitter.md): encoder stages, table-driven pulse shaping, the bank
  schedule, sign windows at 40 MHz, the 8PSK queue at 36 MHz, input, memory.
- [The receiver](receiver.md): capture, matched filter, acquisition and tracking, the
  re-encode check and the LDPC decoder, transport streams, memory.

DVB-S2 is the second-generation satellite broadcast standard,
[ETSI EN 302 307-1 V1.4.1](https://www.etsi.org/deliver/etsi_en/302300_302399/30230701/01.04.01_60/en_30230701v010401p.pdf).
A MODCOD names a modulation and code rate pair, such as 8PSK 2/3. PIE, the S3's Processor
Instruction Extensions, is its 128-bit vector instruction set. The encoding, shaping and
demodulation kernels listed on the two pages run on it; two CRC-32 loops stay scalar C, the
transmitter's input check (`ingress.c`) and the receiver's 5 Msym/s record CRC (`receive.c`).

## Two pipelines, four cores

```
transmit  core 0: payload -> BB frame -> BCH -> LDPC -> map -> PL frame -> frame ring or queue
          core 1: ring or queue -> pulse shaping by table -> DAC bank -> the S3's radio
receive   core 1: the S3's radio -> 16 MS/s dump bank -> unpack -> matched filter -> sync
                  -> symbol ring
          core 0: symbol ring -> derotate, demap -> re-encode check -> (LDPC) -> record or TS
```

Each side splits its work so that one core owns the hard real-time deadline and the other owns
the frames. On the transmitter, core 1 must hand the radio a fresh bank of samples on time,
every time; core 0 only has to finish each frame within its frame period on average, because a
queue sits between them. The receiver is the mirror image: core 1 must empty each capture bank
before the radio comes back round to it, and core 0 has a frame period per frame.

## The radio's two engines

The S3's radio has a test engine that plays samples from internal RAM into its I/Q DAC, and a
dump engine that writes what its receiver hears into internal RAM. Neither is documented by
Espressif; [the credited projects](prior-art.md) mapped both. Both work on 64 KiB SRAM banks
that the CPU and the engine hand back and forth, so each app's real-time budget is "fill or
empty one bank before the engine needs it again":

| Engine | Rate | Bank | Due every | Cycles at 240 MHz |
|---|---:|---|---:|---:|
| DAC, transmit | 40 MS/s | 16,380 samples, 14,268 new | 356.7 µs | 85,608 |
| DAC, transmit | 80 MS/s | 16,380 samples, 15,740 new | 196.75 µs | 47,220 |
| Dump, receive | 16 MS/s | 16,384-sample ring, handed on every 12,288 | 768 µs | 184,320 |

The transmit banks overlap: a bank repeats a lead and a tail of its neighbours, so a switch that
lands a little early or late plays the same samples (`firmware/components/radio_s3`,
[bank geometry](transmitter.md#bank-schedule)). The receive engine writes one continuous
16,384-word ring, and a timer moves it from bank to bank every 12,288 samples
([capture](receiver.md#capture-and-handoff)).

## Shaping by table

The transmitter never does filter arithmetic per sample. A root-raised-cosine pulse is split
into short windows of symbols, every output a window can produce is computed once into a table,
and a sample becomes the sum of a few table words, the same trick as my
[RP2350 version](https://github.com/Rivercraft911/RP2350_IQ_Benchmark/tree/88ab018e65a51805ff562842177a4d47b1b04b2e).
The ordinary pulse spans 9 symbols in three windows of three; the fast modes use shorter pulses
and, at 40 MHz, tables indexed by the signs of QPSK's two axes. [Fill](transmitter.md#fill).

The receiver's matched filter is the opposite trade: it runs real multiply-accumulates, but only
at symbol times, from a bank of fractional-delay coefficient sets
([front end](receiver.md#front-end)).

## No Wi-Fi, no heap

Neither app starts Wi-Fi. At boot, `radio_power_up()` (`firmware/components/platform`) powers the
modem domain, enables the PHY under the Bluetooth modem bit, which runs its full calibration, and
keeps the Wi-Fi baseband clocks on, because both engines live in the Wi-Fi baseband. No Wi-Fi
code is linked into either app. That freed 395 KB of the transmitter image and 407 KB of the
receiver's, and removed Wi-Fi's interrupts and periodic work from both
([transmitter](../results/2026-10-23-tx-no-wifi/README.md),
[receiver](../results/2026-10-23-rx-no-wifi/README.md)).

After boot, no run in either app calls the heap. Every command or run takes its buffers from
regions fixed at link or boot time and gives them back to the next one. ESP-IDF's heap hooks count successful allocations and reallocations and frees of non-null
pointers; a separate callback counts failed allocations; `info`
reports the count, and through the measured paces, keyed runs, dumps, receive runs and benchmarks
it stays at zero ([transmitter](../results/2026-10-23-tx-run-memory/README.md),
[receiver](../results/2026-10-23-rx-no-wifi/README.md)). A few transmitter setup commands are
the exception: `spiinput` installs its SPI driver and CRC table, `spirx` swaps wires, `spisend`
sets up its own SPI master, and `callsign` writes flash with two allocations and two frees
([transmitting](transmitting.md#memory-after-boot)).

## Where the cycles go

The headline modes, with the share of each core they use:

| Mode | Payload | Core 0 | Core 1 | Record |
|---|---:|---|---|---|
| Transmit 8PSK 9/10, 26.67 Msym/s, 36 MHz | 69.8 Mb/s | encoder 78.6% of the frame | slowest fill 79.8% of the bank | [keyed](../results/2026-10-23-tx-table-8psk-rekey/README.md) |
| Transmit QPSK 9/10, 33.33 Msym/s, 40 MHz | 58.2 Mb/s | encoder 73.9% | slowest fill 85.8% | [keyed](../results/2026-10-23-tx-table/README.md) |
| Receive 8PSK 8/9, 5 Msym/s, short frames | 12.64 Mb/s | release 35% + check 28% of the frame | 116.5k of 184,320 cycles per unit, 63% | [sweep](../results/2026-10-23-rx-sweep/README.md) |

Transmit shares are keyed runs with a fresh payload: 1 minus the reserve in the record. The
receive core 0 shares are wall-clock and include each task's preemption by the other, so their
sum is an upper bound.

## Rates

The transmitter's rate token picks the symbol rate, the DAC rate and the pulse.

| Rate token | Symbol rate | Modulation | DAC | Samples/symbol | Pulse span |
|---|---:|---|---:|---:|---:|
| omitted | 1 Msym/s | QPSK | 40 MS/s | 40 | 9 symbols |
| `5m` | 5 Msym/s | QPSK, 8PSK | 40 MS/s | 8 | 9 symbols |
| `10m` | 10 Msym/s | QPSK, 8PSK | 40 MS/s | 4 | 9 symbols |
| `20m` | 20 Msym/s | QPSK | 80 MS/s | 4 | 9 symbols |
| `20m` | 20 Msym/s | 8PSK | 80 MS/s | 4 | 6 symbols |
| `26m667` | 80/3 Msym/s | QPSK | 80 MS/s | 3 | 9 symbols |
| `26m667` | 80/3 Msym/s | 8PSK | 80 MS/s | 3 | 5 symbols |
| `33m333` | 100/3 Msym/s | QPSK | 80 MS/s | 12/5 | 7 symbols |

QPSK MODCODs 4..11 select rates 1/2, 3/5, 2/3, 3/4, 4/5, 5/6, 8/9, 9/10.
8PSK MODCODs 12..17 select 3/5, 2/3, 3/4, 5/6, 8/9, 9/10. Roll-off is 0.35 except at `33m333`,
which uses 0.20. Short frames (`--short`) run QPSK 4..10 at 1 or 5 Msym/s and 8PSK 12..16 at
5 Msym/s. Fractional names are exact ratios; integer telemetry truncates them. The receiver
takes short frames at 1 Msym/s (QPSK) and 5 Msym/s (QPSK and 8PSK).

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

Every encoder stage must match `host/dvbs2.py` bit for bit, and every receiver stage its
host build. The host suite checks the Python reference against standard vectors and
independent stage digests, compares every portable C encoder stage with Python, runs the
transmitter's stream lifecycle with host task stubs against 95 pinned dump CRCs
([fixtures](../tests/data/README.md)), and builds the
receiver's demodulator, FEC and TS deframer for the host against `host/dvbs2.py` frames and a
digital twin of the S3 link.

```sh
python3 -m pip install numpy scipy pyserial matplotlib
for test in tests/test_*.py; do python3 "$test" || exit 1; done
python3 host/plots.py --check                # figure sources and their pinned hashes
(. ~/.espressif/tools/activate_idf_v6.1.sh && cd firmware/tx && idf.py build)
(. ~/.espressif/tools/activate_idf_v6.1.sh && cd firmware/rx && idf.py build)
```

CI ([`.github/workflows/ci.yml`](../.github/workflows/ci.yml)) runs the host tests it lists,
`host/plots.py --rf-only --check`, and builds `firmware/tx` and `firmware/rx` with ESP-IDF v6.1.
Use the [host shell and IDF setup](../README.md#quick-start). If a sandbox blocks ESP-IDF's
unused component manager, use `IDF_COMPONENT_MANAGER=0 idf.py build`; no managed dependencies
are needed.

Host builds assemble no PIE: they run portable C wherever the firmware runs a vector kernel.
Three board benches close that gap, each an ESP-IDF app that runs the real kernels with the
radio off and compares them with portable C or an independent oracle:

| Bench | What it checks | Record |
|---|---|---|
| [`tests/tx_pie_bench`](../tests/tx_pie_bench.c) | the transmitter's PIE kernels: encoder stages, BB inputs at every alignment, xorshift, fills, ring, axes, the 8PSK queue kernels; the slot copy in a later 128-case run; not the Morse ID's 40-sample copy (`station_repeat40`), which only the pinned ID dumps check | [82,493 cases](../results/2026-10-23-tx-pie-bench/README.md), [slot copy](../results/2026-10-23-tx-ts-input/README.md#the-slot-copy) |
| [`tests/rx_demod_bench`](../tests/rx_demod_bench.c) | each demodulator kernel against its C version, timed, the four-level demappers included | [kernel bench](../results/2026-10-22-rx-demod/README.md#board-cycles), [four levels](../results/2026-10-24-rx-decoder-speedups/README.md#the-four-level-kernels) |
| [`tests/rx_fec_bench`](../tests/rx_fec_bench.c) | each FEC kernel against its C version, then whole MODCOD 7 frames; `curve` decodes noisy frames of any short MODCOD as the board does, `setup` times a decode before its first iteration | [decoder near threshold](../results/2026-10-24-rx-fec-curve/README.md) |

```sh
(. ~/.espressif/tools/activate_idf_v6.1.sh && cd tests/tx_pie_bench && idf.py build flash -p PORT)
python3 host/board.py check --reset --timeout 600 --port PORT
```

Timing and memory on real silicon come from the [self-check](../examples/self-check/README.md),
which dumps, decodes and paces the transmitter with the radio off and gates each pace against
the keyed records ([30 of 30 passed](../results/2026-10-23-tx-self-check/README.md)). RF needs a
receiver: the [measurements index](measurements.md) indexes the keyed and received runs.
