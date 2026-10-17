# next5 integration, radio off

Merge `a11b7b5` joins main `4f926aa` and QPSK encoder `8437931`. All eighteen
requested dump CRCs pass the host stream harness. Every generated DAC word is
also checked against the plain encoder, including complete physical-wrap cycles
on both bank geometries. No tap, gain, constellation, payload or waveform changed.
No board port was opened and no firmware was flashed during this integration.

The conflicts retain both sides' routing:

- `fast.c`: direct packed QPSK, direct 4-sps 8PSK, sparse 8-sps 8PSK. QPSK's
  last 720-symbol interval keeps its 16-symbol scalar tail. The sparse fallback
  control loop stays in flash and reuses context scratch.
- `dvbtx.c`: QPSK's complete packed ring, dummy and payload fit after its compact
  table. `20ms6` keeps its three frames after the short table. Legacy 4-sps 8PSK
  keeps two frames in the store and one on the heap. Those allocations belong to
  different commands and never coexist.
- Internal fast workspaces lend their unused tail to the fill window, including
  QPSK at 8 sps. Cleanup frees only separately allocated windows. Allocation
  failure tests cover every allocation in each layout.
- `shaper.c`/`shaper.h`: retain the six-symbol dispatch and add both radix-four
  QPSK kernels. `main.c` keeps QPSK's positional default plus all named rates.
  `20ms6` remains 8PSK only, for dumps, paces and the allocation/Wi-Fi probe.
- Encoder tests combine all 14 modes, both BCH policies, both pilot settings,
  all 16 alignments and guards on the borrowed workspace tail: 896 cases.
  Stream tests combine both rate geometries, all QPSK modes and all CRC gates.

## RAM, derived from allocations and linked sections

All numbers are bytes. Requested heap excludes allocator headers/alignment,
encoder task stack (4,096 B) and other common task/radio costs. The 65,536 B
`shaper_store` is already in static RAM; its used bytes are not new heap.
The DAC banks still reserve 131,072 B. These are sizes, not measured free heap.

| Mode | Store used, maximum | Mode-specific internal heap | Workspace | Fill window |
|---|---:|---:|---|---|
| QPSK default, 40 sps | 32,768 | 17,236–18,100 | 12,420 in PSRAM | 420 heap |
| QPSK `5m` / `10m80`, 8 sps | 50,496 | 29,908–38,100 | 12,420 internal | 2,058 borrowed |
| QPSK `10m` / `20m`, 4 sps | 63,072 | 29,908–38,100 | 12,420 internal | 4,105 borrowed |
| 8PSK `5m` / `10m80`, 8 sps | 65,536 | 46,388–50,938 | 12,420 in PSRAM | 2,058 heap |
| 8PSK `10m` / `20m`, 4 sps | 63,968 | 41,028–48,356 | 12,420 internal | 4,105 borrowed |
| 8PSK `20ms6`, 4 sps | 58,704 | 29,908–37,236 | 12,420 internal | 4,105 borrowed |

Exact per-MODCOD heap components below use the S3 context sizes emitted by the
encoder QEMU harness. `P = (Kbch - 80) / 8 B` comes from
`dvbs2_modes` / `host/dvbs2.py` (EN 302 307-1, Table 5a).
`B` is resident BCH, zero for t=8. All fast QPSK rates request `context + 12420 + B`.
Legacy QPSK requests `context + 420` internally. 8PSK at 4 sps requests
`context + 12420 + B + 11120`; `20ms6` removes the last 11,120 B.
At 8 sps 8PSK requests `context + (23936 + P) + 2058` internally.

| MODCOD | Context | P | B at internal-workspace rates | QPSK fast / 8PSK 4-sps heap | 8PSK 8-sps heap | `20ms6` heap |
|---|---:|---:|---:|---:|---:|---:|
| 4 | 16,816 | 4,016 | 8,192 | 37,428 | n/a | n/a |
| 5 | 16,816 | 4,826 | 8,192 | 37,428 | n/a | n/a |
| 6 | 16,816 | 5,370 | 8,192 | 37,428 | n/a | n/a |
| 7 | 16,816 | 6,041 | 8,192 | 37,428 | n/a | n/a |
| 8 | 17,488 | 6,446 | 8,192 | 38,100 | n/a | n/a |
| 9 | 16,816 | 6,720 | 8,192 | 37,428 | n/a | n/a |
| 10 | 17,488 | 7,174 | 0 | 29,908 | n/a | n/a |
| 11 | 17,680 | 7,264 | 0 | 30,100 | n/a | n/a |
| 12 | 15,568 | 4,826 | 8,192 | 47,300 | 46,388 | 36,180 |
| 13 | 15,184 | 5,370 | 8,192 | 46,916 | 46,548 | 35,796 |
| 14 | 16,624 | 6,041 | 8,192 | 48,356 | 48,659 | 37,236 |
| 15 | 16,528 | 6,720 | 8,192 | 48,260 | 49,242 | 37,140 |
| 16 | 17,488 | 7,174 | 0 | 41,028 | 50,656 | 29,908 |
| 17 | 17,680 | 7,264 | 0 | 41,220 | 50,938 | 30,100 |

For packed storage, `frame = align16(symbols/2 + 16)` gives 16,672 B for QPSK
and 11,120 B for 8PSK; `dummy = align16(3330/2 + 16) = 1696 B`.
QPSK's worst 4-sps occupancy is `4096 + 3*16672 + 1696 + 7264 = 63072 B`,
leaving 2,464 B. At 8 sps it is `8192 + 2*16672 + 1696 + 7264 = 50496 B`.
The worst short 8PSK occupancy is `16384 + align16(2*11120 + 1696 + 7264)
+ 11120 = 58704 B`. The legacy third frame remains on the heap because
`32768 + 31200 + 11120 = 75088 B > 65536 B`.

The workspace tail is `12420 - 8144 = 4276 B`. The largest 4,105 B window leaves
171 B; the 2,058 B window leaves 2,218 B. Fast encoder stages preserve that tail
in all 896 guarded QEMU cases. Plain LDPC cannot use this borrowed layout.

| Linked section | Main `4f926aa` | QPSK `8437931` | next5 |
|---|---:|---:|---:|
| IRAM vectors | 1,028 | 1,028 | 1,028 |
| IRAM text | 93,879 | 94,911 | 96,491 |
| IRAM end padding | 69 | 61 | 17 |
| DRAM data | 30,984 | 35,096 | 30,984 |
| DRAM BSS | 80,592 | 80,592 | 80,592 |

Main and next5 were built locally with identical sdkconfig and IDF v6.1.
The QPSK column is from its existing board-build ELF/map. Compared with main,
every existing mode loses `2612 - 52 = 2560 B` to linked instructions.
Compared with QPSK, linked capacity improves by `4112 - (1580 - 44) = 2576 B`.
Fast QPSK adds no window heap; default QPSK uses 420 B, so its improvement over
that branch is 2,156 B. No mode loses a ring slot, internal workspace, resident
BCH table or mapper choice. Do not count `.dram0.dummy` again: it is the IRAM alias.

Wi-Fi estimates subtract the 2,560 B linked change from main's measured heap:

| Probe | Prior measured up / minimum | next5 estimated up / minimum | Script floor, both |
|---|---:|---:|---:|
| `13 20ms6` | 36,727 / 34,875 | 34,167 / 32,315 | 30,000 |
| `17 5m` | 21,579 / 21,579 | 19,019 / 19,019 | 20,000 |

The `17 5m` gate is predicted to fail by 981 B. Keep and report that failure;
allocation alignment and Wi-Fi allocation order require a board measurement.
Main's 34.9 KB and the QPSK branch's 27.5/37.0 KB summaries used minimum heap,
not the `wifi_free_up` field. The QPSK records actually report up/minimum
29,295/27,479 B (`8 10m`) and 37,307/37,019 B (`11 5m`). Fast QPSK gains
2,576 B of linked capacity relative to that build, predicting 31,871/30,055 B
for `8 10m`; the script conservatively retains its 24,000 B floor.
Sources: [main heap records](../results/2026-10-17-next4/heap.jsonl) and supplied
`qpsk-enc/notes/qpsk-board/heap.jsonl` (selected fields and source hashes are
preserved in the integration records). Estimates do not establish startup success.

## Coordinator script

Run `bash host/next5-board.sh` after the coordinator builds/flashes the clean
checkout. It uses `PY=/Library/Frameworks/Python.framework/Versions/3.13/bin/python3`
and takes about 12 minutes. It prints pass criteria first, runs every command
and every checker despite failures, and finishes with all 29 results in
`summary.tsv`. It exits nonzero if anything failed. `console.log`, JSONL records
and all eighteen saved IQ dumps go in a fresh `notes/next5-board-*` directory.
It reads `info --reset`, requires the current firmware and stored `N0CALL`,
and never writes NVS. A wrong callsign is reported; it is never changed.

The dump list contains main's ten configurations plus QPSK MODCODs 4 and 11
at `5m`, `10m`, `10m80`, `20m`. Firmware CRC, USB CRC, saved-file CRC and exact
binary size must agree. Default QPSK decimation is 10 at 40 sps, 2 at 8 sps,
1 at 4 sps; 8PSK explicitly uses decimation 1.

Paces are exactly `13 5m`, `13 20m`, `13 20ms6`, `4 20m`, `11 20m`, `4`,
30 s each. All require zero late fills/switches, fatal lateness and underruns.
The first five cycle ceilings are floor(measured maximum or mean * 1.02),
using [next4's board run](../results/2026-10-17-next4/README.md) and supplied
`qpsk-enc/notes/board-8437931-pace.jsonl`. The 5m baseline is 228,652 cycles/frame,
not the earlier standalone 222.8k that next4 missed. Slack floors are 45,900,
450, 5,600, 150, 150 cycles respectively, below the prior observed minima.
The default-QPSK gates remain next4's. Limits are printed before each command.
The 20m bank budget is `15740 words * 240e6 cycles/s / 80e6 words/s = 47220 cycles`.
These checks preserve observed behavior; they do not claim 15% fill reserve.

Wi-Fi probes are exactly `8 10m`, `13 20ms6`, `17 5m`. NULL-mode Wi-Fi starts
and stops, with no playback, keying or retuning. Healthy encoder state, the
expected memory layout and both heap floors are checked.

## Offline verification

[Integration records](../results/2026-10-17-next5/README.md) distinguish host,
QEMU, linked-size results and board predictions. All 31 host test programs pass.
LeanDVB also recovers 38 contiguous bit-exact frames from each of the 32
fast-QPSK host configurations, with zero corrections or payload errors.
QEMU passes encoder (140 CRCs, 14 PIE checks, 896 guarded packed frames), shaper
(324 kernel cases, 3,360 sample-glue cases, 39,170 pack/unpack cases, 864 copies,
24 block-pulse cases, 162 short-pulse cases), radio-bank sequencing at both
rates and all 1,760 APSK checks. QEMU establishes correctness, not silicon timing.

The exact requested `idf.py build` was attempted and failed in the unused IDF
component manager: sandboxed `psutil`/`sysctl` process discovery is denied.
`IDF_COMPONENT_MANAGER=0 idf.py build` succeeds with IDF v6.1, as do all harness
builds. No project configuration was changed. The board script itself is tested
with a fake command tool: all 29 commands run, and injected command failure,
wrong saved CRC and insufficient Wi-Fi heap are all reported without stopping.
