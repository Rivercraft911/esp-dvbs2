# Status lines up to 10 Msym/s

The status line now runs only up to 10 Msym/s (commit 5794cca), and there it passes the
radio-off check. At 5 and 10 Msym/s nothing that matters for real time moved: no late fill, no
dummy frame, and the slowest bank fill within 0.5% of main. Each line still costs the encoder
one slower frame, 28k to 47k cycles more than main's slowest. The frame periods there are 268,704
and 532,656 cycles, so even that frame leaves 38.8% or more spare. Above 10 Msym/s a run prints
one status record saying the lines are off. 8PSK 9/10 at 26.67 Msym/s with TS replay is back to
main's numbers: 0 dummies, where the ungated build put in 140 to 162
([status timing](../2026-10-23-tx-status/README.md)). The transmitter stays on this build.

Board: XIAO 44:B1:76:B0:E5:8C, radio off, XIAO overlay, callsign KJ5SCH stored, so every pace
also played both Morse IDs. Cycles are at 240 MHz. The bank deadline is 85,608 cycles at
40 MS/s and 47,220 at 80 MS/s. "Slowest frame" is the run's `encode_cycles_live_max`, and its
spare is against the frame period.

| Pace, 30 s | Build | Slowest fill (spare) | Encoder mean | Slowest frame (spare) | Dummies |
|---|---|---:|---:|---:|---:|
| 8PSK 3/4 short `5m` | main ab1085e, 2 runs | 37,794 to 37,804 (55.8 to 55.9%) | 55,530 to 55,533 | 61,954 to 61,992 (76.9%) | 0, 0 |
| | gated 5794cca, 2 runs | 37,794 (55.9%) | 55,558 to 55,560 | 90,469 to 91,024 (66.1 to 66.3%) | 0, 0 |
| 8PSK 3/4 short `5m`, TS replayed | main | 37,794 (55.9%) | 72,789 | 86,695 (67.7%) | 0 |
| | gated | 37,794 (55.9%) | 72,558 | 133,842 (50.2%) | 0 |
| 8PSK 5/6 `10m` | main, 2 runs | 41,798 to 42,005 (50.9 to 51.2%) | 159,695 to 159,707 | 166,948 to 167,112 (68.6 to 68.7%) | 0, 0 |
| | gated, 2 runs | 41,771 to 41,833 (51.1 to 51.2%) | 157,851 to 157,860 | 195,233 to 196,109 (63.2 to 63.3%) | 0, 0 |
| 8PSK 5/6 `10m`, TS replayed | main | 42,253 (50.6%) | 265,675 | 284,883 (46.5%) | 0 |
| | gated | 42,237 (50.7%) | 266,682 | 325,737 (38.8%) | 0 |
| 8PSK 9/10 `26m667`, TS replayed, lines off | main | 37,480 (20.6%) | 191,933 | 201,142 | 0 |
| | gated | 37,628 (20.3%) | 192,386 | 203,837 | 0 |

No run had a late fill, a late switch or a fatal late, and no status line was dropped. Each 30 s
pace printed 40 lines, 31 of them after the opening ID.

**Encoder means.** At 5 Msym/s they moved by 0.05% and -0.3%, and at 10 Msym/s by -1.2% and
+0.4%. With the lines off at 26.67 Msym/s the mean moved by +0.24%, while two runs on one build
differ by a cycle or two. Lines can only add work, so the lower means are not their doing. The
likeliest cause is the 1,072 bytes of static status memory, which move `shaper_store` and the
stream state up in internal RAM. Memory layout alone has moved encoder means by more than this
before ([pins](../2026-10-23-tx-pins/README.md)). The 26.67 Msym/s encoder keeps 3.7% of its
frame period spare, against main's 3.9%.

**Memory.** `heap_after_boot` read 7 allocations on both builds, all from `spiinput quad ts
replay`'s setup. No pace added any. `main_stack_min_free` went from 1,416 to 1,400 bytes before
that setup, and from 1,320 to 1,288 after it.

**Forwarding.** On this build, `board.py "dvbpace 14 5m 20 --short" --monitor 127.0.0.1:1235`
sent 30 status records and the final record. A UDP listener got all 31 as type 4 datagrams, with
sequence numbers 0 to 30. board.py printed only the final record.

**Still open.** The slow frame after each line is unexplained. The prime suspect is the USB
driver's transmit interrupt running from flash ([status timing](../2026-10-23-tx-status/README.md)).

## Method

Both builds came from the XIAO overlay (`docs/transmitting.md`), flashed with esptool's
watchdog reset, main first. The run order on each build was: `spiinput off`, then
`dvbpace 14 5m 30 --short` and `dvbpace 15 10m 30` twice each. Next came
`spiinput quad ts replay`, then each of those once more, then `dvbpace 17 26m667 30`, and
finally `spiinput off` and `info`. `runs.jsonl` holds every record. `tag` is `base`, `gated` or
`monitor-e2e`, `source` is the commit and `build` is the stamp the firmware reported. Status
lines are the rows with `cmd` `status`. The completion records carry `main_stack_min_free`.
