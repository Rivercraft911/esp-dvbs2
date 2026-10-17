# Two-core experiment

**QPSK 9/10 at 33.333333 Msym/s fails its first silicon pace.** The
[coordinator's 5527cfa records](../results/2026-10-17-moon-silicon/runs.jsonl)
report 49,075 cycles/full bank, 312 underruns and ESP_ERR_TIMEOUT. Minimum
Wi-Fi heap is 38,331 B. The two new 8PSK modes fail table allocation. The
round-four fixes below have host validation and a firmware build, but no new
board measurements. This agent's USB open was denied by the sandbox; RF stayed off.

## Fixes awaiting silicon

The 20 Msym/s 8PSK regression came from a boolean becoming a byte count in
`fill`: `(first & sps)` in a ternary shifted odd-symbol windows by 4 or 8
bytes instead of 1. The packed-ring unpacker then could not reach its aligned
vector loop. The restored boolean preserves the source/output parity.
The stream test fails on the old expression, passes on the fix and still
checks every filled word. Only three prefill frames existed in the failed
20m run, so its 293,140-cycle mean says nothing about steady-state encoding.
The coordinator must remeasure the full bank before this regression is closed.

`dvbpace` now copies an eight-frame precomputed PSRAM ring into the normal
payload buffer. QPSK requests `8 * 7264 = 58112 B` of PSRAM; no additional
internal payload ring is allocated. It repeats after eight frames. Dumps and
RF retain the original contiguous xorshift32 stream, keeping all 57 retained
CRCs and the encoder fixture unchanged. The stage named payload measures the
copy plus station header. `input_prepare_cycles` reports startup generation
outside the paced interval. This measures a memory copy, not SPI ingress.
Native tests compare both sides of the input-ring wrap to the plain encoder
and cover allocation failure/cleanup.

The helper wrapper now resides in instruction RAM and runs at priority 24,
above Wi-Fi's priority 23. IDF v6.1 `docs/en/api-guides/performance/speed.rst`
documents Wi-Fi priority; `components/esp_system/esp_ipc.c` gives IPC priority
24 too. The old priority-6 helper could be delayed by Wi-Fi while core 1
waited. Interrupts, equal-priority IPC, context switches and SRAM contention
remain possible delays. This is a latency change to measure, not a timing bound.

Each encoder stage now reports helper-body cycles inside that same window.
Core-0 CCOUNT and helper totals are paired under a short local interrupt mask;
the cache-counter reads remain outside it. `cycles - helper_cycles` retains
scheduler, interrupt and memory stalls. Cache counters still include both
cores and interrupts. `encoder_pie` must be 15, confirming all four fast-stage
bits. `encode_frame_work_cycles_*` includes the measured input copy and kernel,
subtracting only helper bodies. `host/stream_profile.py` reports stage wall,
helper and remaining cycles; old records without helper attribution stay unknown.

## What the model missed

The [round-three instruction model](../results/2026-10-17-moon-round3/README.md)
estimated 38,948 core-1 cycles/bank and 13,379 helper-body cycles/bank. Silicon
reached 49,075 and 24,660 respectively. Dispatch max 2,356 fits its 2,396-cycle
allowance, but join wait reaches 11,394 cycles. Dispatch completion did not
mean the helper could immediately run. The model also omitted coprocessor
context switching, Wi-Fi task interference and SRAM arbitration; the old
1,068-cycle tick allowance does not bound those delays.

The QPSK stage means were payload 107.0, BB 12.0, BCH 85.6, LDPC 46.3, map
42.4, PL 4.2 kcycles/frame. BCH/LDPC/map have only 5.10 / 2.97 / 4.99 flash
misses/frame in shared observation windows. Those counters do not establish
table eviction as the cause of doubled wall times. The same build's QPSK
26m667 kernel mean is 96,735 cycles/frame, with BCH 44,549 cycles/frame.
The split's post-payload mean is 192,276 and remaining work 142,256 cycles/frame.
The new per-stage helper accounting distinguishes preemption on the next run.

The exact bank deadline remains
`15740 samples / 80e6 samples/s * 240e6 cycles/s = 47220 cycles`.
The 15% reserve ceiling is 40,137 cycles. QPSK's frame period is
`33282 symbols / (100e6/3 symbols/s) * 240e6 cycles/s = 239630.4 cycles`;
its 85% encoder ceiling is 203,685 cycles/frame. Frame dimensions follow
host/dvbs2.py and EN 302 307-1 V1.4.1 clauses 5.1/5.5. Payload generation
alone used 106,956 cycles/frame in the failed split. Replacing it creates
capacity to measure; it cannot certify the remaining work or worst-case jitter.

## Coordinator check

Build/flash the committed revision, with N0CALL stored and all other serial
owners closed, then run:

```sh
PYTHON=/Users/river/radioconda/bin/python3 \
RX=/Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb \
  host/check.sh --moonshot notes/moon-round4-board
```

`--plan` opens no device. The check refuses busy native/reset-bridge aliases
before reset/open, keeps session ownership, logs each command, continues on
failures and exits nonzero for any failure. No flashing, NVS writes or TX.
All legacy ceilings and heap floors stay unchanged. The core-0 screen now
uses actual same-frame work including input:
`helper_bank_max + 4792 scheduling + max(frame_work_mean, frame_work_live_max)
* 47220 / frame_period <= 40137 cycles/bank`. It remains a capacity screen,
not a latency proof. The full bank must independently pass <=40,137 cycles,
zero lateness/underruns, positive slack and >=20,000 B Wi-Fi-up/minimum heap.
The two new 8PSK gates remain expected failures; the legacy 5m heap floor is
also still unresolved. No mode is admitted from the instruction model.
