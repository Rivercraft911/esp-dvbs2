# Buildout: 8PSK frames in internal RAM

8PSK 2/3 and 5/6 now hold every bank deadline with 49% slack at 5 Msym/s (main: 6%) and 32%
at 10 Msym/s (main stops after 2 late fills), over 60 s with the CW ID and the radio off. Every
dumped sample is byte-identical to main's. The encoder takes 257k cycles per 8PSK 2/3 frame
instead of 705k. In the worst 8PSK mode, 32.7 KB of internal RAM is still free once Wi-Fi is up;
Wi-Fi's own start-up needs at most 17.8 KB. QPSK timing is unchanged within run-to-run spread.
No `tx`, `dvb` or `chain` command was sent; nothing was keyed.

## What ran

ESP32-S3 at 240 MHz on `/dev/cu.usbmodem101`, radio off, N0CALL stored (so every paced run
adds two 5.22 s CW IDs: 70.4 s in all). Branch `exp/buildout`:

| Firmware | What | Records |
|---|---|---|
| `4b35a04` | main, the baseline (built twice: in this worktree, then from `git archive` with `version.txt` for an A/B) | `main.jsonl` |
| `f51aff5` | merge of exp/rd-fill, before the changes below | `steps.jsonl` |
| `4fa303c`, `0074771`, `e1dcf59` | intermediate builds, kept because they show each fix | `steps.jsonl` |
| `0e9c3ef` | final | `runs.jsonl`, `fec.jsonl`, `dumps.jsonl` |

LeanDVB decodes of main's and the final dumps are in `decodes.jsonl` (LeanDVB executable
SHA-256 `5482ab74...`, as in results/2026-10-16-stream; `host/onair.py --stream 40 --mer
--callsign N0CALL`). Dumps are in ignored `notes/buildout/`. Each record file starts with
an `info` record naming its firmware.

## Correctness

| Check (0e9c3ef) | Result |
|---|---|
| `fecbench pie` | all 140 CRCs equal the reference; 14 `pie_check` records with mismatch 0, now including packed PIE and C frames against the packed C phases for the six 8PSK MODCODs |
| `dvbdump 4 40` | CRC 1435641601, equal to main |
| `dvbdump 13 5m 40 128 1` | CRC 1937985440, equal to main |
| `dvbdump 15 5m 40 128 1` | CRC 1163322913, equal to main |
| `dvbdump 13 10m 40 128 1` | CRC 474263962, equal to main |
| `dvbdump 15 10m 40 128 1` | CRC 1757765864, equal to main |
| the five dump files | byte for byte identical to main's (`cmp`) |
| LeanDVB, all five | contiguous bit-exact payload with the 40-byte ID header: 38, 37, 36, 36, 36 frames, 0 LDPC flips, 0 BCH failures; MER 43.82, 43.78, 43.78, 40.66, 40.67 dB, as on main |
| `dvbdump 13 10m 40 128 1 1000`, `dvbdump 15 5m 40 128 2 1000` | encoder stalled 1 s per frame: 148 and 155 packed dummy PLFRAMEs inserted; LeanDVB locked throughout and decoded 16 and 15 frames bit-exact and contiguous |
| `iddump` | CRC 1965033598 as in results/2026-10-16-station-id; Morse decodes N0CALL |

Host side: all 25 `tests/test_*.py` pass; `idf.py build` has no warnings; the QEMU harness
passes its 324 shaper cases, 1,520 sample-glue cases and 13,058 new pack/unpack cases with
hashes equal to the host.

## Timing

Cycles at 240 MHz. A bank is due every 85,608 cycles. Slack is the smallest margin to a
bank switch over the run; 15% is 12,842 cycles. Frame period: 1,065,312 cycles at 5 Msym/s,
532,656 at 10, 7,987,680 for QPSK at 1.

| Run | Firmware | Longest fill | Smallest slack | Late / fatal / underruns | Encoder mean / max per frame | Min lead (symbols) |
|---|---|---:|---:|---|---:|---:|
| `dvbpace 13 5m 60` | main | 79,937 | 5,422 (6.3%) | 0 / 0 / 0 | 704,700 / 728,752 | 132,835 |
| | final | **42,936** | **42,403 (49.5%)** | 0 / 0 / 0 | **257,401** / 444,548 | 14,781 |
| `dvbpace 15 5m 60` | main | 79,595 | 5,745 (6.7%) | 0 / 0 / 0 | 771,393 / 788,559 | 130,788 |
| | final | **42,845** | **42,496 (49.6%)** | 0 / 0 / 0 | **376,092** / 453,272 | 12,996 |
| `dvbpace 13 10m 60` | main | 107,158 | stopped after 5.2 s | 2 / 1 / 0 | 666,529 / 682,065 | |
| | final | **57,164** | **28,175 (32.9%)** | 0 / 0 / 0 | **257,361** / 433,451 | 29,569 |
| `dvbpace 15 10m 60` | main | 113,594 | stopped after 5.2 s | 2 / 1 / 0 | 731,210 / 740,931 | |
| | final | **57,634** | **27,703 (32.4%)** | 0 / 0 / 0 | **376,045** / 448,328 | 26,002 |
| `dvbpace 4 60` (QPSK) | main | 41,011; 40,656 | 44,326; 44,685 | 0 / 0 / 0 | 708,029 / 767,244 | 228,388 |
| | final | 41,246 | 44,090 (51.5%) | 0 / 0 / 0 | 699,272 / 759,969 | 228,469 |
| `dvbpace 13 10m 30 128 10` | final | 55,582 | 29,757 | 0 / 0 / 71,895 (stall on purpose) | 257,392 | 0 |

The encoder mean is the steady state. The maximum is probably a start-up frame with cold caches;
per-frame times are not recorded. Main's QPSK was run twice to measure its spread (355
cycles); the final QPSK run lies 235 to 590 cycles above those.

Where the cycles go now (final records, `profile`, per 16,380-word bank on average):

| Rate | Copy and unpack | Shaping | Partial symbols | Whole fill |
|---|---:|---:|---:|---:|
| 5 Msym/s | 3,325 | 32,493 | 666 | 36,740 |
| 10 Msym/s | 4,796 | 42,998 | 0 | 47,999 |
| QPSK | 3,875 | 28,932 | 706 | 33,791 |

Encoder stages per 8PSK 2/3 frame: payload generation 43.5k, BB 7.7k, BCH 99.3k, LDPC 37.8k,
map and pack 65.8k, PL 3.1k. With frames in PSRAM the same frame took BB 150k and map 227k
(rd-fill's baseline profile, results/2026-10-16-rd-fill-board), because both cores were
missing in the shared data cache.

## Memory

The transmitter runs inside internal RAM that Wi-Fi also needs. Measured on the final
firmware with `wifimem` (allocate a mode's memory as `dvb` does, then start Wi-Fi exactly as
`dvb` does without keying; see docs/stream-profile.md):

| Step | Internal heap |
|---|---:|
| `esp_wifi_init` (2 static RX buffers, Wi-Fi code in flash, 81da950) | 15,420 to 15,876 B |
| start, power save off, promiscuous, channel 1 | 108 to 116 B |
| transient below the started level (idle probe) | 1,816 B |
| Wi-Fi's start-up need, at most | 17.8 KB |

Free internal RAM with Wi-Fi up next to each layout (`wifi_free_up`; with a transmitter
allocated it equalled the low-water mark), and the largest free block:

| Mode | Ring in internal heap | Free with Wi-Fi up | Largest block |
|---|---:|---:|---:|
| 8PSK 2/3, 5 Msym/s | 29,308 B | 37,067 B | 24,576 B |
| 8PSK 5/6, 5 Msym/s | 30,656 B | 34,371 B | 24,576 B |
| 8PSK 9/10, 5 Msym/s (largest payload) | 31,200 B | **32,679 B** | 23,552 B |
| 8PSK 2/3, 10 Msym/s | 11,120 B | 55,255 B | 31,744 B |
| 8PSK 5/6, 10 Msym/s | 11,120 B | 53,911 B | 31,744 B |
| 8PSK 9/10, 10 Msym/s | 11,120 B | 52,759 B | 31,744 B |
| QPSK 1/2 | 0 (PSRAM) | 64,747 B | 31,744 B |
| nothing allocated | | 85,335 B | 31,744 B |

For comparison, main's keyed run (results/2026-10-16-keyed-box) reached 41,895 B free in
8PSK 2/3 at 5 Msym/s; the same mode here has 4.8 KB less, and every 10 Msym/s mode has more
than main had at 5. The original start-up failure happened at 8,574 B free. The worst case
leaves 32.7 KB, 1.8 times Wi-Fi's whole start-up need, with a 23.5 KB largest free block.

Where the RAM came from. Idle free internal RAM went from 86,763 B (main) to 109,431 B:

- statics that one command uses moved out of DRAM: `tx`'s 13,476 B test signal now uses the
  shaping table's store (commands run one at a time and every DVB command rebuilds its table);
  the `dvbdump` and `iddump` USB buffers (8,212 B) are allocated in PSRAM when used; `usbrx`
  allocates its 4,096 B buffer on use; the 1,440 B tap scaling buffer is a PSRAM temporary.
- 8PSK frames are packed two phases per byte (11,097 B per frame), and at 10 Msym/s two of the
  three frames, the dummy and the payload fit in the half of the 64 KiB table store that the
  4-sps table leaves unused. rd-fill used a 22,194 B heap block per frame instead.
- costs: 3,124 B more code in internal RAM (`idf.py size`, DIRAM .text 70,907 to 74,031 B: the
  packed paths, the unpack kernel and rd-fill's notification) and a 554 B overlap cache.

## How it got there

The merge with exp/rd-fill had only additive conflicts (dvbtx.c, shaper.h, the component
CMakeLists): the payload callsign header now runs before rd-fill's first profile mark, and the
record prints the callsign before rd-fill's ring and profile fields. The host stream harness
gained station-ID stubs. rd-fill's board results are in results/2026-10-16-rd-fill-board.

Each later step was found on the board:

1. **The last bank.** On the merge, 10 Msym/s runs had a longest fill of 79,716 cycles in both
   MODCODs. A diagnostic build that records where the slowest bank starts (now the
   `max_fill_sample` field) put it 860 samples before the end of a 10 s run: after the final
   ID, `station_id_fill` padded the rest of the bank one volatile word at a time, about 5
   cycles per sample. It now uses the CW path's 128-bit copies (`309add4`).
2. **Packed frames everywhere** (`63e3156`..`4fa303c`). 13 5m fell from 79.9k to 44.5k and the
   encoder from 705k to 255k on the first try.
3. **Profile cost.** `4fa303c` gave QPSK 43,147 against main's 41,011. A diagnostic build
   without dvbpace profiling gave 41,535 and showed the encoder notification costs the fill up
   to 2,348 cycles. Fill phases in timed runs now read CCOUNT only, and QPSK polls again
   (`0074771`).
4. **Lead at 10 Msym/s.** With two frames the 5/6 encoder kept 3,808 symbols (0.38 ms) of
   lead. A third frame (11,120 B of heap) raised it to 26,002 (`e1dcf59`).
5. **QPSK's PSRAM layout.** QPSK was still 1.0 to 1.2k above main in `0074771` and `e1dcf59`.
   Its slowest call is the PSRAM copy (9.6k cycles against a 3.9k mean), whose cost depends on
   which data cache sets the slots share with the encoder's workspace, and the ring was being
   allocated before the workspace. Allocating in main's order gave 41,246 (`0e9c3ef`).

| Firmware | 13 5m longest / slack | 15 10m longest / slack / lead | QPSK longest |
|---|---|---|---:|
| `4b35a04` main | 79,937 / 5,422 | fails | 41,011; 40,656 |
| `f51aff5` merge, 10 s runs | | 79,716 / 5,645 / 3,808 | |
| `4fa303c` packed | 44,529 / 40,806 | 58,279 / 27,061 / 3,808 | 43,147 |
| `0074771` profile, notify | 43,514 / 41,825 | 58,041 / 27,298 / 3,808 | 42,210 |
| `e1dcf59` three frames at 10m | 43,254 / 42,082 | 57,760 / 27,578 / 26,002 | 42,011 |
| `0e9c3ef` final | 42,936 / 42,403 | 57,634 / 27,703 / 26,002 | 41,246 |

The two diagnostic builds were not committed and reported a stale version string; their
numbers above are evidence for the changes, not results.

## Not shown, and risks

- **No RF.** Keyed runs, the DAC engine's reads of bank memory and Wi-Fi activity during a run
  were not tested here; the coordinator runs those. `dvb` records now carry the `wifi_*` heap
  fields from the real start-up.
- **Wi-Fi start-up in the probe** starts the PHY on channel 1 with its calibration, in NULL
  mode with promiscuous receive, inside the shielded box. It never keys or retunes, but it is
  not "radio off" in the sense of the paced runs.
- **Encoder lead with Wi-Fi on core 0.** At 5 Msym/s two frames keep 13.0k to 14.8k symbols
  (2.6 to 3.0 ms) of lead; at 10 Msym/s three keep 26.0k to 29.6k (2.6 to 3.0 ms). A longer
  stall on core 0 inserts dummy PLFRAMEs (the payload is delayed, not lost); it does not miss
  a bank deadline.
- **Encoder sensitivity to layout.** The same 5/6 encoder took 365k cycles in `0074771` and
  376k to 380k after later builds moved code and data; its flash data misses per frame in BCH
  rose from 79 to 90-102. 2/3 did not move. Not investigated.
- **dvbpace still profiles.** Cycle-only marks cost little, but the QPSK comparison rests on one
  final run against two main runs.
- The PIE unpack and pack were checked in QEMU and, for packing, on silicon by `fecbench`;
  the unpack runs on silicon in every dump and paced run above, and the dumps are identical.

## Next: keyed, in the shielded box (coordinator)

`dvb 2420 13 5m 30 4 128`, `dvb 2420 15 5m 30 4 128`, `dvb 2420 13 10m 30 4 128`,
`dvb 2420 15 10m 30 4 128`, then `dvb 2420 4 30 0 128`. Pass: `ESP_OK`, 0 late fills, 0 fatal,
0 underruns, longest fill at most 72,766 and slack at least 12,842 cycles, `fe_gain_index` 0,
`wifi_min_free` above 20 KB (this run's probe gives 32.7 to 64.7 KB). Record `wifi_*`,
`min_free_internal`, `largest_internal` and `min_lead_symbols`.
