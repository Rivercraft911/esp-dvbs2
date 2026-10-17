# Encoder

Core 0 continuously encodes normal DVB-S2 frames. QPSK MODCODs 4..11 and 8PSK
12..17 share BB framing, BCH and LDPC error correction. `host/dvbs2.py` is the
independent bit-level reference. Firmware stages must match it bit for bit.
The format is SIS/CCM, generic continuous stream, pilots on, roll-off 0.35,
PL scrambling code 0. The library also tests transport-stream input and pilots off.

The equations and constellation labels come from EN 302 307-1: BB framing in
5.1, BCH/LDPC in 5.3.1/5.3.2 and Table 5a, interleaving/mapping in 5.3.3/5.4,
and PL framing in 5.5. The generated LDPC addresses are from Annex B.
[Prior art and standard source](prior-art.md).

## Stages and memory

`dvbs2.c` implements the plain C reference. `fast.c` reorganizes the same stages;
`pie.S` supplies the ESP32-S3's 128-bit Processor Instruction Extensions (PIE).
Portable C remains the host oracle and fallback for these data layouts.

- BB: copy payload, write the 80-bit header and scramble in one pass. TS uses
  table CRC-8 and keeps packet state across frames.
- BCH: use byte recurrence with an 8,192 B resident table for t=10/12 when the
  workspace is internal. Fold 16 input bytes into the remainder before reducing.
  Internal t=8 uses two 4,096 B flash tables, reducing two bytes per step.
  PSRAM workspaces keep register-based nibble rows. Earlier t=10/12 flash byte
  tables lost in the live stream. [Cache effects](../results/2026-10-16-next/README.md).
- LDPC: form doubled 360-bit information groups; XOR shifted byte windows into
  parity rows, prefix-XOR, transpose and pack into canonical MSB-first bits.
- Map: fuse interleaving, constellation mapping and PL scrambling. Internal
  workspaces use direct packed output. 8PSK at 5 Msym/s uses sparse lookups
  whose hot mapping footprint is 512 B beside its PSRAM workspace.
- PL: insert the prepared header and pilots. QPSK at 20 and 80/3 Msym/s writes
  four `phase>>1` codes per byte, oldest in bits 7:6. Other fast rates write
  pairs, first phase in the low nibble. Default 1 Msym/s uses raw phase bytes.
  The QPSK vector copy preserves the aligned store's prefix: a nine-byte pilot
  can share that store with the preceding run's data.

Payload is a continuous xorshift32 stream, seed 2026. Each frame overwrites
its first 40 payload bytes with the stored station ID. Dummy frames consume no
payload. In the QPSK block modes, payload borrows encoder scratch until BB
consumes it; BCH/LDPC/map can then reuse that scratch.

A workspace is 12,420 B on this target. Fast stages use only the first 8,144 B;
the remaining 4,276 B holds the fill core's symbol window. Plain LDPC uses that
tail and must never run concurrently with the borrowed window. Contexts,
workspaces, tables and vector destinations are 16-byte aligned. Requested sizes
exclude allocator overhead; see `layout` and `*_heap_bytes` in stream records.
[Fill layouts](bank-fill.md) explain which allocations share the table store.

Both cores share cache. Cache counters count requests and misses, not stall
cycles or exclusive per-core activity. Internal SRAM contention is absent from
those counters. `host/encoder_memory.py` estimates touched cache lines from
linked symbols; it does not predict execution time. Source: ESP32-S3 TRM v1.8,
4.3.3.2 and Table 4.3-2, p. 405.

## Timing and checks

Measured on main: 8PSK 2/3 at 10 Msym/s took 167,519 cycles/frame mean and
168,955 live maximum; QPSK 9/10 at 80/3 Msym/s took 227,692 mean and 228,865
live maximum. These include payload generation and profiling.
[8PSK records](../results/2026-10-17-next4/runs.jsonl),
[QPSK records](../results/2026-10-17-qpsk-goal/runs.jsonl).

For frame length N symbols and symbol rate Rs symbols/s, one core has
`240e6 cycles/s * N / Rs` cycles/frame. QPSK has 33,282 symbols with pilots;
8PSK has 22,194 (5.5, Table 11). Thus QPSK at 80/3 Msym/s has 299,538
cycles/frame and 8PSK at 20 Msym/s has 266,328. Payload throughput is
`(Kbch - 80) * Rs / N` bit/s, before any application framing.

`fecbench` defaults to `pie`: the shipped packed mapper, internal workspace
and resident BCH policy. `fecbench plain` is the bit-level reference. Source,
CRCs and printing are outside the timer; canonical phase bytes are recovered
from packed output after timing. `dvbpace` is the complete-stream timing gate,
including the sparse/PSRAM path, ring traffic and bank schedule.

`tests/test_dvbs2_c.py` compares every stage with Python. `test_fecbench.py`
checks all 140 seeded phase CRCs and rejects incomplete records. Its reader
still accepts historical benchmark labels so old evidence remains verifiable.
The encoder QEMU harness checks real PIE instructions, both packed formats,
both BCH policies, pilots and guards. The EE.LDXQ.32 address probe remains:
measured silicon follows the TRM; Espressif QEMU differs by 4 B.
[Measurement](../results/2026-10-16-fec-limit/README.md).
QEMU and instruction models establish no silicon timing.

## Encoder candidate for 33.33 Msym/s

At `Rs = 100e6/3 symbols/s`, the frame budgets are 159,796.8 cycles for 8PSK
and 239,630.4 for QPSK. A 15% reserve gives integer ceilings 135,827 and
203,685 cycles/frame. FEC/map work does not depend on samples/symbol or roll-off.
The wider waveform, its RAM cost and the matching BBHEADER roll-off bits must
be integrated separately; this candidate retains roll-off 0.35.

For t=8, with 128-bit remainder R and generator g from 5.3.1:
`R' = (R << 16) XOR T0[head0 XOR byte0] XOR T1[head1 XOR byte1]`, truncated
to 128 bits, where `T0[v] = v*x^136 mod g`, `T1[v] = v*x^128 mod g`.
All message lengths are even. Zero input keeps a zero remainder; full ones
and random frames match the independent bit-serial reference. Preloading a
message vector uses linearity: XOR it into the first 128 remainder bits, then
reduce as if the next 16 input bytes were zero. Tail bytes use the original
recurrence.

`encode_kernel_cycles_mean` and `encode_kernel_cycles_live_max` time one
continuous interval from the end of payload generation through PL framing,
including station ID and profiling. They exclude ring waits. Startup is excluded
only from the live maximum. Adding 10,000 cycles is an **input-copy estimate**,
not a measurement of SPI or a maximum formed by subtracting stage maxima.

After the coordinator builds/flashes the committed revision, run:

```sh
host/check.sh --encoder --plan
PYTHON=/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 \
  RX=/path/to/leandvb host/check.sh --encoder notes/moon-encoder-board
```

This keeps going through six decoded dumps, FEC CRCs, four 30 s fast paces,
one 10 s PSRAM regression and three Wi-Fi probes. It prints every criterion
first. Both post-payload mean and live maximum plus 10k must meet the ceilings
above. Wi-Fi up and low-water free RAM must each reach 20,480 B. The 9/10
8PSK dump is 16 frames, CRC 3,782,178,445. Existing CRCs are unchanged. The
real SPI stream must later meet the same total-cycle ceilings without substitution.

The Python reference's 16APSK/32APSK formulas and independent digest/loopback
tests remain. APSK has no firmware transmitter mode. Its unused firmware
shaping prototypes were removed.
