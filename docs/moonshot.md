# Two-core experiment

**Compact QPSK 9/10 at 33.333333 Msym/s passes the instruction model.**
It has not passed silicon timing or Wi-Fi heap admission. The coordinator's
interrupted e0766d4 run is invalid and supplies no split timing evidence.
This session opened no serial port and performed no flash or radio operation.

Both cfd012c and its round-two follow-up 73136ce are merged. The compact
27,648-byte QPSK kernel uses the seven-symbol roll-off 0.20 pulse. Its support
is `Rs*(1+alpha) = (100e6/3)*1.20 = 40e6 Hz`. The payload estimate is
`(100e6/3)*58112/33282 = 58.202 Mb/s`, excluding station-ID pauses, input
starvation and application framing. Frame dimensions come from host/dvbs2.py
and EN 302 307-1 V1.4.1 clauses 5.1/5.5. The pulse and DAC-hold/alias mask
checks are in host/moon_fill.py; this is not an RF spectrum measurement.

The original uint16 input kernel is fast enough only if packing is omitted
from the bank account. The integrated QPSK loop instead reads quad bytes
straight from the ring window, retaining history in a register. Four blocks
consume five bytes and produce 48 samples in 116 modeled cycles. Each block
still adds the same three independently rounded table rows. Edges use the
original block API. No waveform arithmetic changes in the fused loop.

Core 1 stages an immutable window and publishes one release/acquire job at a
time to a priority-6 helper on core 0, above the priority-5 encoder. QPSK gives
2/7 of each fresh piece to the helper, rounded to twelve samples; 8PSK gives
1/3. Both cores join before window reuse, overlap retention or bank return.
A physical bank wrap can dispatch twice. Only core 1 reads ring state.
Encoder work telemetry subtracts preempting helper bodies on core 0; cycle
counters from different cores are never subtracted.

The deadline is `15740 samples / 80e6 samples/s * 240e6 cycles/s = 47220 cycles`
per core; the 15% reserve target is 40,137 cycles. host/moon_schedule.py
replays assembled instructions and enumerates all 819 physical bank offsets.
It now counts the final two instructions inside expanded long loops correctly.

| Mode, 9/10 | Pulse | Core 0, cycles/bank | Core 1, cycles/bank | Result |
|---|---|---:|---:|---|
| QPSK 33m333 | 7 symbols, .20 | 38,903 | 38,948 | Model passes |
| 8PSK 26m667 | 6 symbols, .35 | 50,469 | 53,660 | Timing/RAM fail |
| 8PSK 33m333 | 7 symbols, .20 | 60,441 | 62,897 | Timing/RAM fail |

These are estimates, not worst-case timing guarantees. Encoder demand uses
99,787 QPSK or 101,466 8PSK cycles/frame from the instruction-model records in
results/2026-10-17-moon-encoder/runs.jsonl, including an estimated 10,000-cycle
input copy. Frame periods are 239,630.4 / 199,746 / 159,796.8 cycles respectively.
Core 0 demand is `encoder*47220/frame + helper + jobs*2396 + 1068` cycles/bank.
Core 1 is `max(local,helper) + staged bytes + copied samples/2 + jobs*2396 + 2068`.
The 2,396-cycle dispatch allowance comes from the earlier measured notification
maximum rounded upward (results/2026-10-17-qpsk-goal); 1,068 tick and 1,000
control cycles are allowances. QPSK additionally reserves 1,024 cycles/chunk
for at most eight edge blocks and wrapper work. SRAM contention, MULL
throughput, Wi-Fi interrupts and these allowances need measurement. The helper
body estimate reaches 13,379 cycles/bank, rather than the isolated 5.4k proposal.

QPSK tables, three frames, dummy and window request
`27648 + 25056 + 864 + 1744 = 55312 B` inside the existing 65,536-byte store.
Its internal workspace/context remain 12,420 / 17,712 B; the helper requests
2,048 B plus its task control block. Its actual Wi-Fi headroom is unknown.

8PSK tables request 73,728 B (3 sps) or 76,800 B (2.4 sps). Two whole parts
share the store with a frame and dummy. Extra tables plus ring/window request
53,680 / 56,752 B of heap. Against the closest cleaned-main Wi-Fi headroom of
47,291 B, reaching a 20,000 B floor needs at least 26,389 / 29,461 B reclaimed,
before helper and linked-RAM changes. This comparison is a planning screen.
A symmetry identity alone does not provide a fast restoration kernel. No new
compressed format is retained without a timing path that can use it.
The fast 3-sps kernel really uses .35, occupying 36 MHz at 80/3 Msym/s.
At 2.4 sps, packing costs another 21 cycles/block; even zero kernel work leaves
core 0 at 45,583 cycles with this split. It needs smaller tables and less
packing/staging/dispatch work, as well as a faster full bank.

All 57 retained waveform CRCs and the encoder fixture pass. Six new complete
streams match Python at every sample and through full bank wraps. Their CRCs
change intentionally with the new experimental pulses, table factoring and
3-sps BBHEADER. LeanDVB recovers 14 contiguous QPSK / 13 fractional 8PSK /
14 three-sps 8PSK frames with zero FEC corrections or payload errors. The
receiver gets 4,096 zero samples after the pinned dump to drain lookahead.
QEMU passes 324 block cases, 15,552 slices and 20,736 direct-quad cases; QEMU
clocks are never used as silicon measurements.

After building/flashing the committed revision, with N0CALL stored:

```sh
PYTHON=/Users/river/radioconda/bin/python3 \
RX=/Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb \
  host/check.sh --moonshot notes/moon-round3-board
```

`--plan` opens no devices. The runner locks ownership across worktrees and
checks native cu/tty aliases and the reset bridge before reset/open. It writes
revision.json and board-info.json, keeps every command log and continues after
failures with summary.tsv and a nonzero exit. The lock coordinates board.py
users; unrelated tools must still leave the board alone. The coordinator must
measure complete banks <=40,137 cycles, positive slack, no late fills/switches
or underruns, exclusive encoder/helper capacity, and Wi-Fi-up free/minimum heap
>=20,000 B. The two 8PSK gates are expected to fail. No real-time admission is
claimed until the board passes.
