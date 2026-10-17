# Encoder

Core 0 continuously encodes normal DVB-S2 frames. QPSK MODCODs 4..11 and 8PSK
12..17 share BB framing, BCH and LDPC error correction. `host/dvbs2.py` is the
independent bit-level reference. Firmware stages must match it bit for bit.
The format is SIS/CCM, generic continuous stream, pilots on, roll-off 0.35,
PL scrambling code 0. Experimental `33m333` selects GCS roll-off 0.20 with
`DVBS2_RO_20`; all defaults keep 0.35. The library also tests transport-stream input and pilots off.

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
  workspace is internal. Use register-based nibble rows for t=8 and PSRAM
  workspaces. The flash byte-table and copy-per-frame experiments lost in the
  live stream. [Measured cache effects](../results/2026-10-16-next/README.md).
- LDPC: form doubled 360-bit information groups; XOR shifted byte windows into
  parity rows, prefix-XOR, transpose and pack into canonical MSB-first bits.
- Map: fuse interleaving, constellation mapping and PL scrambling. Internal
  workspaces use direct packed output. 8PSK at 5 Msym/s uses sparse lookups
  whose hot mapping footprint is 512 B beside its PSRAM workspace.
- PL: insert the prepared header and pilots. QPSK at 20 and 80/3 Msym/s writes
  four `phase>>1` codes per byte, oldest in bits 7:6. Other fast rates write
  pairs, first phase in the low nibble. Default 1 Msym/s uses raw phase bytes.

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

The Python reference's 16APSK/32APSK formulas and independent digest/loopback
tests remain. APSK has no firmware transmitter mode. Its unused firmware
shaping prototypes were removed.
