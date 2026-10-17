# next4 integration, radio off

The host stream preserves all ten requested dump CRCs. Both bank geometries,
both packed mappers and the borrowed workspace tail pass their correctness
checks. No serial port, board or radio was used for this integration. Timing and
Wi-Fi heap on the combined firmware remain for the coordinator.

`a7e55f2` merges `exp/max-encoder` at `0ec9964`. `f00537a` merges
`exp/max-astra` at `ba549b4`. The integration starts from main `32ea843`.
`c75812a` adds the requested `wifimem 13 20ms6` allocation probe and its tests.

The five conflicted files were resolved as follows:

- `dvbtx.c`: retain main's 40/80 MS/s geometry and sparse mapper at 8 samples
  per symbol (sps), direct mapper and resident BCH at 4 sps. Adopt the six-symbol
  table, frames in the table store, borrowed workspace window and suffix-only
  sample-cache saves. Keep main's keyed legacy rates; `20ms6` stays radio off.
- `main.c`: preserve every existing token and argument position, add `20ms6`
  to dumps and pacing, reject it for keyed `dvb`. The later Wi-Fi probe change
  accepts it solely for allocation plus Wi-Fi NULL-mode start/stop.
- `encoder_pie/main/app.c`: combine both mappers, resident/nibble BCH, every
  alignment and the workspace-tail guards, giving 384 complete frames.
- `stream_host.c` and `test_stream.py`: combine both geometries, new pulse,
  allocation failure cleanup, full physical-wrap cycles and explicit CRC gates.

No tap, constellation, scaling, payload or legacy waveform was changed.
The two new pulse CRCs match Astra's supplied board records.

## Internal RAM

These are linked sizes and requested allocations, not measured free heap.
Both builds use ESP-IDF v6.1, the same generated sdkconfig and main `32ea843`
as the baseline. [Offline records](../results/2026-10-17-next4/README.md).

| Section, bytes | Main | Integrated | Change |
|---|---:|---:|---:|
| IRAM text | 92,311 | 93,879 | +1,568 |
| IRAM end padding | 101 | 69 | -32 |
| DRAM data | 35,096 | 30,984 | -4,112 |
| DRAM BSS | 80,592 | 80,592 | 0 |

Static internal capacity saved is `4112 - (1568 - 32) = 2576 B`. The end of
DRAM BSS moves down by that same 2,576 B. Do not also count `.dram0.dummy`,
which reserves the data-address alias of the instruction allocation.

The sparse mapper adds no heap allocation and keeps its control loop in flash.
Its temporary phases use the context's existing scratch, independently of the
4-sps window in the FEC workspace tail. That tail has
`12420 - 8144 = 4276 B`; a 4,105 B window fits with 171 B left. Only fast encoder
stages may run concurrently with this window. Plain LDPC uses the parity array.

| Profile | New requested heap | Removed heap | Net capacity saved |
|---|---:|---:|---:|
| Legacy 4 sps | 0 B | 0 B | 2,576 B |
| `20ms6` | 0 B | 11,120 B third frame | 13,696 B |
| 8 sps, including `5m` | 2,058 B window | 0 B | 518 B |
| QPSK | 420 B window | 0 B | 2,156 B |

The short table and packed storage use 56,816 B for MODCOD 13 and 58,160 B for
MODCOD 15 inside the existing 65,536 B store. The workspace stays 12,420 B, fast
contexts stay 15,184/16,528 B, and resident BCH stays 8,192 B for those modes.
Heap headers, alignment and changed addresses can alter actual free heap and
timing. The two Wi-Fi probes check the short layout and the largest 8-sps mode.

## Coordinator checks

After one build/flash of the clean branch, run:

```bash
bash host/next4-board.sh
```

The script sets `PY=/Library/Frameworks/Python.framework/Versions/3.13/bin/python3`,
uses a new output folder, and should take about 10 minutes. It checks the flashed
version and the already-stored `N0CALL` before running. It never writes NVS.
All ten dumps use `--save`; their firmware CRCs, host CRCs and binary sizes are
checked. QPSK uses its default decimation of 10; all 8PSK dumps use decimation 1.
The script lists every command and its pass criteria before running it.

All paced runs require `ESP_OK`, no encoder failure, zero underruns, zero late
fills/switches/fatal lateness, correct mapper and workspace policy, and conserved
sample counts. Cycle gates are fixed here before the integrated board run:

| 30 s pace | Fill maximum, cycles | Encoder mean, cycles/frame | Encoder live maximum, cycles/frame | Minimum slack, cycles |
|---|---:|---:|---:|---:|
| 13 5m | 45,200 | 227,257 | 234,116 | 1 |
| 15 5m | 45,200 | 260,161 | 265,713 | 1 |
| 13 10m | 60,000 | 170,493 | 172,099 | 1 |
| 13 20m | 46,249 | 170,371 | 173,506 | 700 |
| 13 20ms6 | 41,276 | 172,482 | 180,589 | 5,671 |
| 15 20ms6 | 41,278 | 190,932 | 194,981 | 5,671 |
| 4 | 43,457 | 695,717 | 701,401 | 1 |

Encoder ceilings are the supplied 0ec9964 or ba549b4 measured means/live maxima
times 1.02, rounded down. The 20m row uses
[main's radio-off record](../results/2026-10-16-20msym/runs.jsonl) instead.
Legacy fill ceilings retain the earlier integration limits in
[max-encoder.md](max-encoder.md); 20m and short-pulse fills allow 2% above their
measured maxima. The short slack floor subtracts 810 cycles from the supplied
6,481-cycle minimum; the legacy 20m floor subtracts 908 from 1,608 cycles.
Nonfatal legacy switch lateness in the older encoder branch is not grandfathered
in: main's interrupt policy is retained, and this integration requires zero.

These gates test preservation of measured behavior. They do not certify Astra's
earlier 15% reserve: its supplied 40,467/40,469-cycle fills already exceed the
40,137-cycle gate in [round2.md](round2.md) by 330/332 cycles. At 80 MS/s and
240 MHz, each bank permits `15740 words * 240e6 cycles/s / 80e6 words/s =
47220 cycles`. Both the original gate and this integration's limits are below
that deadline. Startup maxima remain in the records; only the reported ring
prefill is excluded from the encoder's live maximum.

Both `wifimem 13 20ms6` and `wifimem 17 5m` require successful Wi-Fi start/stop,
healthy encoder state and at least 20,000 B of internal heap both while Wi-Fi is
up and at its low-water mark. The probe returns before TX keying, retuning or
sample playback. It uses the requested encoder layout; the unchanged Wi-Fi
probe's own station-ID allocation remains at 40 MS/s.
