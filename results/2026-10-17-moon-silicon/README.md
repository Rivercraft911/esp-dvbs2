# First split timing on silicon

QPSK 9/10 at 100/3 Msym/s fails real-time admission. These are the
coordinator's power-cycled, TX-off measurements of firmware
5527cfac0611575e8a786cca9de8de620d09fff7, copied from notes/moon-round3-board/runs.jsonl.
No measurement in this directory was produced by the round-four agent.
The original complete check reported 14 passed and 11 failed.

[runs.jsonl](runs.jsonl) preserves all five pace and four Wi-Fi memory records,
including allocation failures. The source host timestamps and commands are
retained; firmware_revision identifies the tested source. These records are
immutable. There are no new board measurements for the parity/input fixes.

| Pace, 30 s requested | Full-bank maximum, cycles | Encoder mean, cycles/frame | Result |
|---|---:|---:|---|
| QPSK 9/10, 26m667 | 39,204 | 155,266 | ESP_OK |
| 8PSK 5/6, 20m | 50,063 | 293,140 | ESP_ERR_TIMEOUT |
| QPSK 9/10, 33m333, Wi-Fi up | 49,075 | 297,746 | ESP_ERR_TIMEOUT |

The 20m failure has only three prefill frames and no live encoder maximum.
Its mean is not a steady-state encoder measurement. The parity regression
is explained in [the current account](../../docs/moonshot.md).

For QPSK 33m333, the full-bank maximum exceeds the 38,948-cycle instruction
model by 10,127 cycles (26.0%). The helper's worst bank is 24,660 cycles,
versus 13,379 modeled. Dispatch reaches 2,356 cycles, within the 2,396-cycle
allowance, but join wait reaches 11,394 cycles. The run reports 312 underruns,
2 late fills, 1 fatal lateness and minimum lead 5 symbols. Wi-Fi minimum free
is 38,331 B. Neither timing nor encoder capacity passes.

Stage means are payload 106,956; BB 12,023; BCH 85,602; LDPC 46,272;
map 42,351; PL 4,189 cycles/frame. BCH/LDPC/map flash misses are 5.10 / 2.97 /
4.99 per frame. Counters count both cores and interrupts in overlapping
windows. They do not isolate cache eviction from helper preemption. Existing
encoder-work telemetry subtracts helper bodies, leaving scheduler, interrupt
and memory stalls in the remaining 142,256 cycles/frame.

Both new 8PSK modes fail table allocation. Legacy 8PSK 9/10 at 5 Msym/s
has 21,571 B Wi-Fi minimum, below its unchanged 22,351 B regression floor.
No threshold was relaxed. RF spectrum and receiver sensitivity remain unknown.
