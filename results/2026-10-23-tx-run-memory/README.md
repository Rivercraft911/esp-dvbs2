# Transmitter run memory: no heap after boot

**Result.** After boot the transmitter makes no heap call in any run. ESP-IDF's heap hooks
counted 0 allocations and 0 frees through ten commands on 60e722f: five paces (including VCM),
`fecbench`, `iddump`, a dump and a 10 s keyed run. The only later heap use came from the
`callsign` command, whose NVS write made 2 allocations and 2 frees. All 22 paces keep their
deadlines, with no late fills and no dummies, and 93 of the 95 golden dumps are exact. The
other two, 8PSK 5/6 and 9/10 at 26.67 Msym/s, are known to be unstable (below).

Firmware: 899244a (run memory) for the paces, 60e722f (899244a plus the dump watchdog fix) for
the dumps and heap counts. The baseline is the static-stacks tree, recorded as `90ea0c4-dirty`
and committed as 20cbe5b.

## Layout

Every command takes its buffers from `firmware/tx/main/run_memory.c`, and the next command
returns everything at once.

| Region | Address | Bytes | Largest use measured |
|---|---|---:|---|
| Internal, low | 0x3fce0000, just above the DAC banks, up to the ROM startup stack | 38,672 | 38,640 B |
| Internal, high | 0x3fcf0000, the DRAM half of a 32 KB data cache | 32,768 | 25,920 B |
| PSRAM | one block taken from the heap at boot | 393,216 | 294,848 B (QPSK 1 Msym/s dump) |

A take goes to the first internal region with room. The largest internal total in one run was
57,088 B, in the 8PSK 3/4 10 Msym/s dump. The encoder and radio worker stacks are static,
and the radio worker is created once at boot on core 1. That worker holds core 1's
task-watchdog subscription, so a run no longer adds and deletes watchdog entries (2 heap calls
per run before). The 8PSK 26.67 Msym/s queue no longer checks a heap floor. Heap free after
boot is 38,982 B, and runs leave it untouched.

## Paces (10 s each, xorshift payload, radio off)

Sets `pace-static-stacks` and `pace-run-memory` in `runs.jsonl`. Internal and PSRAM columns
are the run-memory bytes each run took.

| Mode | Encoder mean, static stacks | Run memory | Change | Internal taken (B) | PSRAM taken (B) | Late fills | Dummies |
|---|---:|---:|---:|---:|---:|---:|---:|
| 11 | 716,486 | 710,686 | -0.8% | 18,128 | 290,736 | 0 | 0 |
| 11 5m | 161,807 | 161,851 | +0.0% | 38,832 | 1,440 | 0 | 0 |
| 11 10m | 161,563 | 161,576 | +0.0% | 38,832 | 1,440 | 0 | 0 |
| 4 20m | 130,965 | 130,824 | -0.1% | 53,152 | 1,440 | 0 | 0 |
| 11 20m | 130,325 | 128,265 | -1.6% | 45,824 | 1,440 | 0 | 0 |
| 4 26m667 | 130,923 | 130,725 | -0.2% | 36,448 | 1,440 | 0 | 0 |
| 11 26m667 | 130,301 | 128,162 | -1.6% | 29,120 | 1,440 | 0 | 0 |
| 4 33m333 | 180,185 | 180,106 | -0.0% | 43,536 | 1,440 | 0 | 0 |
| 11 33m333 | 177,585 | 177,522 | -0.0% | 46,688 | 1,440 | 0 | 0 |
| 17 5m | 271,549 | 279,525 | +2.9% | 50,704 | 13,872 | 0 | 0 |
| 17 10m | 247,357 | 236,850 | -4.2% | 49,952 | 1,440 | 0 | 0 |
| 13 20m | 142,873 | 142,485 | -0.3% | 30,704 | 1,440 | 0 | 0 |
| 15 20m | 164,001 | 165,601 | +1.0% | 32,048 | 1,440 | 0 | 0 |
| 17 20m | 246,690 | 261,732 | +6.1% | 33,200 | 1,440 | 0 | 0 |
| 13 26m667 | 188,425 | 185,978 | -1.3% | 50,048 | 18,128 | 0 | 0 |
| 15 26m667 | 198,051 | 195,524 | -1.3% | 52,736 | 17,936 | 0 | 0 |
| 17 26m667 | 179,495 | 177,006 | -1.4% | 50,336 | 17,552 | 0 | 0 |
| 7 --short | 153,863 | 146,148 | -5.0% | 5,744 | 85,632 | 0 | 0 |
| 10 5m --short | 50,945 | 50,938 | -0.0% | 35,312 | 1,440 | 0 | 0 |
| 16 5m --short | 66,084 | 66,079 | -0.0% | 23,440 | 13,872 | 0 | 0 |
| vcm 1 4 7 11 4 | 958,243 | 952,215 | -0.6% | 0 | 292,656 | 0 | 0 |
| vcm mixed 4 5m | 384,955 | 368,491 | -4.3% | 0 | 17,984 | 0 | 0 |

Encoder means are cycles per frame. They moved by -5.0% to +6.1%, the same spread as
earlier layout changes. 8PSK 9/10 at 20 Msym/s is the tight case. Its 261,732-cycle mean
leaves 1.7% of the 266,328-cycle frame.

## Dumps

Set `dumps` (60e722f): 93 of 95 golden CRCs match. 8PSK 5/6 (15/3/80/16) and 9/10 (17/3/80/16)
at 26.67 Msym/s do not, and they change from run to run. In those dumps the decision to send
a dummy frame races the core-0 encoder against the core-1 dump at each frame boundary: queued
pages and the producer's phase. The goldens record one outcome of that race, and the faster
table placement here changes it. Their dummy counts were 0 + 8 and 2 + 21 before. Now they
vary: 0 + 3, 0 + 6, 0 + 9, 2 + 14. The stream itself is valid: LeanDVB decodes a varying 9/10
dump to 12 contiguous exact payload frames with no FEC corrections. Making these two dumps
deterministic belongs to the 8PSK work (dumps that wait for the encoder, with CRCs pinned
from the host model). Until then they are known-unstable, not failures.

Set `dumps-arena-in-psram` is a measurement build that kept every run's memory in PSRAM. Its
take order, sizes and zeroing were the same. All 95 dumps matched there, including those two,
with the slower encoder of that build.

## Heap counts

Set `heap`: after a reset, `info` reports `heap_after_boot` before and after the ten commands
above. Each command's completion record also carries `heap_allocs_after_boot`.
