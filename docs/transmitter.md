# The transmitter

One ESP32-S3 makes every DVB-S2 frame and every output sample itself. Keyed for 30 s with a
payload that never repeats, 19 selected configurations delivered anything from 0.97 Mb/s (QPSK
1/2 at 1 Msym/s) to 69.8 Mb/s (8PSK 9/10 at 26.67 Msym/s), and every frame LeanDVB recovered
from the sampled bursts was exact
([keyed table](../results/2026-10-23-tx-table/README.md),
[8PSK re-key](../results/2026-10-23-tx-table-8psk-rekey/README.md)). Core 0 encodes frames;
core 1 shapes them into the radio's sample banks. Two numbers decide whether a mode works: the
encoder's mean cycles per frame against the frame period, and the slowest bank fill against the
bank deadline. I keep at least 15% of each spare where I can, and say where I can't.

The app is `firmware/tx`: console, commands and stream control in `main/`, with the encoder in
`firmware/components/dvbs2`, the pulse tables and fill kernels in `components/shaper`, the radio
in `components/radio_s3`, payload input in `components/ingress` and the Morse ID in
`components/station_id`. The overview, rates and checks are in [How it works](how-it-works.md).

## Encoder

The encoder follows [ETSI EN 302 307-1 V1.4.1](https://www.etsi.org/deliver/etsi_en/302300_302399/30230701/01.04.01_60/en_30230701v010401p.pdf),
with pilots on and physical-layer scrambling code zero. Normal frames are the default;
`--short` selects short frames for QPSK 4..10 at 1 or 5 Msym/s and 8PSK 12..16 at 5 Msym/s.

1. **Payload.** The synthetic test payload is xorshift32 seeded with 2026, its first 40 bytes the
   stored callsign. SPI or USB can supply MPEG transport stream (TS) or generic stream (GCS)
   input instead ([payload input](#payload-input)).
2. **BB frame.** An 80-bit header records the input type and data length with a CRC-8, the
   payload follows, and the frame is scrambled (§§5.1, 5.2). TS input also replaces each
   packet's sync byte with the CRC-8 of the packet before it.
3. **BCH** adds 192, 160 or 128 parity bits on a normal frame (t = 12, 10 or 8) and 168 on a
   short one; **LDPC** then fills the 64,800-bit (normal) or 16,200-bit (short) FEC frame
   (§5.3, Tables 5a to 7b, Annexes B and C).
4. **Map.** QPSK puts two bits on one of four phases, 8PSK three on one of eight, after a
   column interleaver (§§5.3.3, 5.4).
5. **PL frame.** A 90-symbol header, 36-symbol pilot blocks and the PL scrambling sequence
   (§5.5). A normal frame with pilots is 33,282 QPSK or 22,194 8PSK symbols; a short frame 8,370
   or 5,598.

`host/dvbs2.py` is the reference, and every C stage must match it bit for bit. `dvbs2.c` is a
plain portable encoder; `fast.c` is the one the radio runs, with packed bits, tables and PIE
kernels (`pie.S`, `ts_pie.S`, `qpsk_signs.S`).

**Where the cycles go.** Per-stage means on the board, radio off, cycles per frame at 240 MHz:

| Mode | Frame period | Payload | BB | BCH | LDPC | Map, PL, pages | Encoder mean | Spare | Record |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 8PSK 9/10, 26.67 Msym/s, fresh payload | 199,746 | 28,060 | 3,079 | 43,897 | 19,836 | 62,244 | 157,218 | 21.3% | [tx-8psk](../results/2026-10-23-tx-8psk/README.md) |
| 8PSK 9/10, 20 Msym/s, fresh payload | 266,328 | 28,061 | 3,216 | 44,566 | 24,173 | 22,485 | 124,130 | 53.4% | [tx-8psk-20m](../results/2026-10-23-tx-8psk-20m/README.md) |
| QPSK 9/10, 33.33 Msym/s, TS input replayed | 239,630 | 38,430 | 27,788 | 43,959 | 23,997 | 68,546 | 202,990 | 15.3% | [tx-ts-input](../results/2026-10-23-tx-ts-input/README.md) |

What got each stage there:
- **Payload.** xorshift32 runs four lanes per 128-bit vector, with lane jumps from 512 bytes of
  tables: about 28k cycles for 7,264 bytes. With TS input, most of the cost is checking the
  wire, not copying it: the PV check (header, CRC-32 and TS syncs) takes 34.3k cycles a frame,
  the CRC-32 near the floor of its slicing-by-4 loop, and the slot copy 1.9k.
- **BCH.** Table steps instead of bit loops: a byte at a time for t = 10 and 12, two bytes from
  pair tables for t = 8. For t = 10 the remainder sits one byte up in its register, so both table
  indices come from one `EXTUI` each, 14 instructions per byte pair
  ([tx-8psk](../results/2026-10-23-tx-8psk/README.md#changes-in-order)).
- **LDPC.** The standard's parity addresses in groups of 360 become whole-word and vector XORs,
  and the parity pack is unrolled for q = 18, 30 and 60.
- **Flash.** The encoder's tables must not miss the 32 KiB data cache. 8PSK 9/10 at 20 Msym/s
  read 8 KiB of BCH pair tables from flash, overran the cache and took 310 flash misses a frame
  at about 490 cycles each; with the tables in internal run memory it went from 254,280 to
  124,130 cycles ([tx-8psk-20m](../results/2026-10-23-tx-8psk-20m/README.md)).

`fecbench` times BCH and LDPC alone on one frame
([budget](../results/2026-10-23-tx-budget/README.md#isolated-fec-fecbench)).

## Payload input

Without input the encoder makes its own payload. `spiinput` selects a wire instead: single-line
SPI, quad SPI, native USB, or a replay of eight presealed messages through the real parser
(a capacity test that needs no sender). Messages are PV v1: 1,332 bytes, a 12-byte header, up to
seven 188-byte TS packets and a CRC-32, paced by a READY pin
([SPI input](spi-input.md)). The ingress component checks each message, copies its payload out of
the slot with a PIE kernel, and hands the encoder a byte stream; when the input runs dry, the
transmitter sends a standard dummy PLFRAME and the frame grid carries on.

`spiinput` installs the SPI driver, or the quad bus and its GDMA channel, when it selects the
wire, and `spiinput off` frees them; the USB wake timer is made at boot. Runs only start and stop
them, so a run with input makes no heap call either.

Capacity is not the same as a live link. Replayed TS input keeps QPSK 9/10 at 33.33 Msym/s at
15.3% encoder spare, and 8PSK 9/10 at 26.67 Msym/s at full rate with 2.5 to 3.9% spare, both
keyed and decoded exact ([TS input](../results/2026-10-23-tx-ts-input/README.md)). No real wire
has delivered those rates on the board: quad SPI was tested to 46.6 Mb/s
([SPI input](spi-input.md)). The live video so far is a CM5's 9 Mb/s camera mux over 16 MHz SPI
([live RF](../results/2026-10-19-live-rf/README.md)).

## Fill

The DAC takes signed 10-bit in-phase (I) and quadrature (Q) values, one complex sample per
32-bit word: I in bits 9:0, Q in bits 19:10. A root-raised-cosine pulse shapes the symbols
(§5.6 and Annex A). For N samples per symbol, symbol x[k] and pulse h in DAC LSB units:

```
y[k*N+p] = sum(a=0..L-1, x[k-a] * h[a*N+p])       DAC LSB
```

The ordinary pulse spans L = 9 symbols. A table for every possible 9-symbol history would be
far too large, so the pulse is split into three windows of three symbols, each with its own
table indexed by its symbols. A sample is three table words added together, then one XOR with
0x80200 turns the packed sum into two signed fields without carries between the axes. Integer
taps are in 1/256 DAC LSB, constellation coordinates in 2^-30, and each window is rounded after
division by 2^38. The 8PSK 20 Msym/s pulse spans six symbols in two windows. The taps include
inverse-sinc correction for the DAC's sample hold (Annex A).

At 80 MS/s QPSK uses four-symbol blocks, three tables and 256 windows per table:
`3 * 256 * 4 * N * 4 B = 12288*N B`, 49,152 B at N = 4 and 36,864 B at N = 3. The symbol timeline
runs straight through frame boundaries.

Frames sit in internal RAM, compressed: two phases per byte for 8PSK and slower QPSK, four for
QPSK at 80 MS/s. Core 0 publishes finished slots; core 1 frees a slot only after its last needed
symbol. Samples already finished in a bank's overlap are reused rather than shaped twice. If the
ring runs dry, a valid DVB-S2 dummy frame keeps the frame grid, and the run counts an underrun.

Measured keyed with a fresh payload, the slowest fill leaves 49.7 to 55.5% of the bank period
spare at 40 MS/s and 14.2 to 23.5% at 80 MS/s
([keyed table](../results/2026-10-23-tx-table/README.md),
[8PSK re-key](../results/2026-10-23-tx-table-8psk-rekey/README.md)). The fill kernels are pinned to their
position in a 32-byte fetch line by the linker, because moving a loop by 12 bytes once cost the
8PSK 20 Msym/s fill 1.0% ([pins](../results/2026-10-23-tx-pins/README.md)).

## Bank schedule

The DAC test engine loops a 16,380-sample bank until the CPU switches it. Two 64 KiB banks at
0x3fcc0000 and 0x3fcd0000 are reserved by the linker. The engine keeps its sample offset across a
switch, so sample n lives at word `n % 16380` in either bank, and each bank also holds a lead
before its start and a tail after its end: near a switch, either bank plays the same samples.

```
lead + step + tail = 16380                         samples
deadline = step * Fcpu / Fs                        CPU cycles
```

| Fs | Lead | Tail | New samples (step) | Period | Deadline at 240 MHz |
|---|---:|---:|---:|---:|---:|
| 40 MS/s | 64 | 2,048 | 14,268 | 356.7 µs | 85,608 cycles |
| 80 MS/s | 128 | 512 | 15,740 | 196.75 µs | 47,220 cycles |

The 40 MS/s lead and tail came from esp-sdr-zephyr's scheduler. At 80 MS/s I measured my own:
the switches that used to land late were the 1 kHz FreeRTOS tick on the fill core, so normal
interrupts are now masked through the short wait and the switch, and the switch aims half a lead
early ([20 Msym/s](https://github.com/Rivercraft911/esp-dvbs2/blob/archive/dev-2026-10-17/results/2026-10-16-20msym/README.md)).
The fill itself runs with interrupts on and counts their cost.

**Keying.** The radio is powered and calibrated at boot without Wi-Fi
([How it works](how-it-works.md#no-wi-fi-no-heap)). For each run the worker tunes, starts the
vendor PHY's CW test tone and stops it, which brings the whole transmit chain up with calibrated
settings and leaves the DAC on the data path (esp-sdr-zephyr's method,
[credits](prior-art.md)), pins the gain, and restores the analog filter: the CW setup resets
PBUS(4,2), so wide signals get 32 back and narrow ones 0, and the readback goes in the run's
record. One PLL and temperature tracking pass, about 44,000 cycles (0.18 ms), runs before keying
([no Wi-Fi](../results/2026-10-23-tx-no-wifi/README.md)). Without Wi-Fi nothing repeats that pass;
in a 535 s keyed run, six 0.5 s bursts sampled from start to end recovered 355 frames, every one
exact; best-frame MER spanned 17.2 to 18.2 dB, and the last burst read 0.2 dB below the first
([drift](../results/2026-10-23-tx-no-wifi-checks/README.md)).

A run lasts at most 540 s including both Morse IDs (`RADIO_S3_MAX_SECONDS`), and the console
refuses to key without a stored callsign ([transmitting](transmitting.md)).

## Sign windows and 40 MHz

At 100/3 Msym/s the 80 MS/s DAC makes 12 samples for every five symbols, and a seven-symbol
pulse at roll-off 0.20 gives `(1+0.20)*(100/3) MHz = 40 MHz`. That is the widest channel the
S3's Wi-Fi radio was built for, and it took a different renderer.

So QPSK becomes two BPSK streams. Each four-sample output vector depends on eight signs per axis,
so complete, already rounded I and Q values come from small 256-row tables, one per axis, and
combine with one OR. For the three vectors of a twelve-sample group the sign windows are
[-6..1], [-5..2] and [-3..4]. No independently rounded pulse fragments are ever added.

The encoder queues compact I/Q bit planes, headers and pilots included: forty words per page,
sixteen signs per axis per word. Core 1 pulls the eight-sign windows straight out of those
rolling bit streams while rendering, on a fixed 80-symbol schedule of table loads, packed adds
and stores; the DAC fields never overlap, so adding is exactly OR. Before playback the queue
holds two whole frames. A full queue blocks the encoder until the renderer frees eight pages,
and one notification per bank wakes it.

One 65,536 B shaping store holds `24576 + 105*160 + 768 + 8144 = 50288 B`: axis tables, 105
queue pages, a 192-sample scratch chunk and the encoder workspace. Each page covers 128
five-symbol groups, 1,536 samples, so the queue covers `105*1536 / 80e6 s = 2.016 ms`.

Keyed with a fresh payload, QPSK 9/10 at 40 MHz keeps 26.1% encoder spare but only 14.2% fill
spare, just under my 15% bar; with TS input replayed it keeps 15.3% and 15.4%
([keyed table](../results/2026-10-23-tx-table/README.md),
[TS input](../results/2026-10-23-tx-ts-input/README.md)).

## 8PSK at 36 MHz

At 80/3 Msym/s, 8PSK uses a five-symbol pulse at roll-off 0.35, `(1+0.35)*(80/3) MHz = 36 MHz`,
and carries the most data of any mode: 69.8 Mb/s at 9/10. Core 0 publishes compact four-symbol
descriptors and core 1 renders them straight into the DAC banks.

The encoder keeps its BCH, PRBS, Gold and mapper tables in internal RAM so that it never fights
the renderer for the flash cache, and the renderer's small control path is locked into one 4 KiB
way of the instruction cache while this mode runs. One 65,536-byte store holds the whole pulse
table, the queue and the stream mux:

```
36864 table bytes + 54 * 528 page bytes + 160 mux bytes = 65536 bytes
```

Each page holds 128 four-symbol descriptors, up to 1,536 samples. A 22,194-symbol frame needs 44
pages, and the ten spare pages let the next frame's burst of mapping overlap playback. The depth
was found by sweep, not by the mean: at 46 pages the encoder lost airtime to dummies that 54 pages
removed ([page sweep](measurements/2026-10-18-8psk-page-sweep.jsonl)). Payload, BB, BCH and LDPC
for the next frame run before it waits for pages; only mapping and PL publication block.

**Admission.** A keyed or paced fill never waits. At each PLFRAME boundary it admits a whole
frame, or a partial frame that a measured production bound says will arrive in time. Let G be the
largest recurring page-publication gap after bulk mapping (excluding full-queue sleep), W = 2G
cycles per page, P the ready pages, and R = 9 reserve pages. The wake allowance D is zero during
mapping or 47,220 cycles while awaiting a bank-end wake. Every missing page k = 1 to 44-P must
satisfy:

```
D + k*W <= (P-R+k-1)*2944                         CPU cycles
```

The renderer needs at least `128*23 = 2944` single-issue instructions per page, and the
inequality is linear, so only its endpoints are checked. If admission fails, a standard
3,330-symbol dummy PLFRAME (EN 302 307-1 §5.5.1) keeps the frame grid; no data frame or input
byte is skipped. A radio-off dump is not real time, so its fill instead waits for the whole next
frame and never inserts a dummy, which makes its bytes independent of encoder speed
([fixtures](../tests/data/README.md)).

Keyed with a fresh payload, the three rates keep 20.2%, 16.5% and 21.4% encoder spare and 19.8
to 20.4% fill spare, with no dummies
([re-key](../results/2026-10-23-tx-table-8psk-rekey/README.md)). Before the encoder work of
[tx-8psk](../results/2026-10-23-tx-8psk/README.md) they ran at 1.8 to 9.8% spare, radio off.

## PIE kernels

The kernels below run on PIE. The PV CRC-32 that checks input stays scalar C (`ingress.c`),
slicing by 4. [`tests/tx_pie_bench`](../tests/tx_pie_bench.c) checks the kernels on the board
against portable C or an independent oracle: 82,493 cases, every configuration the transmitter
uses exact ([PIE bench](../results/2026-10-23-tx-pie-bench/README.md)). The slot copy came later
and has its own 128-case run ([TS input](../results/2026-10-23-tx-ts-input/README.md#the-slot-copy)).
The Morse ID's 40-sample copy, `station_repeat40` (`components/station_id/station_copy.S`), is
checked only through the pinned ID dumps.

| Where | Kernels |
|---|---|
| `components/dvbs2`: `pie.S`, `ts_pie.S`, `qpsk_signs.S` | BB scrambling and CRC-8, the TS packet CRC-8, BCH byte, pair and shift steps, LDPC rows, transposes and parity pack, mapper runs, QPSK sign planes |
| `tx/main/payload_pie.S` | xorshift32, four lanes per vector |
| `components/shaper` | fills at 4, 8 and 40 samples per symbol, the QPSK radix-4 quads, the short pulse, the 40 MHz axis renderer, the 8PSK descriptor prepare, render and mapper |
| `tx/main/ring.c` | packed frame unpack and two-bit copies |
| `components/ingress/ingress_pie.S` | the slot copy, one unaligned load, `EE.SRC.Q.QUP` and store per 16 bytes |

The bench found one wrong kernel path: with short 8PSK frames, the generic PIE mapper gets the
last 12 to 40 symbols wrong. The transmitter never takes that path; short 8PSK runs only at
5 Msym/s through the sparse packed mapper, which was exact, and its pinned dumps match.

## Memory

No Wi-Fi code is linked, and after boot no run calls the heap; only the setup commands
`spiinput`, `spirx`, `spisend` and `callsign` do ([transmitting](transmitting.md#memory-after-boot)).
Each command takes
its buffers from fixed run memory (`firmware/tx/main/run_memory.c`), and the next command returns
everything at once ([run memory](../results/2026-10-23-tx-run-memory/README.md)):

| Region | Bytes | Largest use measured |
|---|---:|---|
| Internal, just above the DAC banks | 38,672 | 38,640 B ([run memory](../results/2026-10-23-tx-run-memory/README.md)) |
| Internal, the DRAM half of the 32 KB data cache | 32,768 | 31,616 B (QPSK 1/2 at 20 Msym/s, TS replayed, [tx-8psk](../results/2026-10-23-tx-8psk/README.md)) |
| PSRAM, one block taken at boot | 393,216 | 294,848 B (QPSK 1 Msym/s dump, [run memory](../results/2026-10-23-tx-run-memory/README.md)) |

The largest internal total recorded in one run is 67,936 B, a `dvbpace 4 20m` with TS input
replayed (36,320 B low plus 31,616 B high, [tx-8psk](../results/2026-10-23-tx-8psk/README.md));
the run-memory campaign's own largest was 57,088 B, an 8PSK 3/4 10 Msym/s dump. Encoder and radio
worker stacks, their task blocks and the Morse ID state are static, and the radio worker lives on
core 1 from boot. ESP-IDF's heap hooks (successful allocations, reallocations and non-null frees;
failed allocations counted separately) count zero after boot through paces, keyed runs, dumps,
`fecbench` and `iddump`.

Dropping Wi-Fi raised the internal-RAM low-water marks of the radio-off paces by 19 to 37 KB and
let 8PSK 5/6 at 26.67 Msym/s start at all; with Wi-Fi it stopped with `ESP_ERR_NO_MEM`
([no Wi-Fi](../results/2026-10-23-tx-no-wifi/README.md)).
