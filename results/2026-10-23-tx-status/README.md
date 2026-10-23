# The status line costs the encoder one slow frame a second

The once-a-second status line (`dvb` and `dvbpace`, commit 937ef5b) does not pass the radio-off
timing check. Each line costs the encoder one slow frame: 24k to 50k cycles more than its slowest
frame on main. At 8PSK 9/10 and 26.67 Msym/s with TS input replayed, where the encoder has 2.5
to 3.9% to spare, that put 162 and 140 steady dummy frames (and 2 startup dummies each) into two
30 s paces. Main put in none. Late fills stayed at zero everywhere, and the encoder means moved
by 0.01 to 0.3%. Writing the line without printf did not remove the slow frame. The board went
back to main afterwards.

Board: XIAO 44:B1:76:B0:E5:8C, radio off, XIAO overlay, callsign KJ5SCH stored, so every pace
also played both Morse IDs. Cycles are at 240 MHz. The bank deadline is 47,220 cycles at 80 MS/s.

| Pace, 30 s | Build | Slowest fill (spare) | Encoder mean | Slowest frame | Steady dummies |
|---|---|---:|---:|---:|---:|
| 8PSK 9/10 `26m667` | main ab1085e, 3 runs | 36,664 to 36,764 (22.1 to 22.4%) | 157,046 to 157,047 | 162,489 to 162,961 | 0, 0, 0 |
| | status 937ef5b, 3 runs | 38,581 to 40,526 (14.2 to 18.3%) | 157,063 to 157,064 | 197,857 to 199,485 | 0, 0, 0 |
| QPSK 9/10 `33m333` | main, 3 runs | 38,924 to 38,953 (17.5 to 17.6%) | 167,653 to 167,654 | 171,256 to 171,318 | none possible |
| | status, 3 runs | 38,969 to 39,399 (16.6 to 17.5%) | 167,744 to 167,745 | 195,072 to 198,961 | none possible |
| 8PSK 9/10 `26m667`, TS replayed | main, 2 runs | 37,465 to 37,505 (20.6 to 20.7%) | 191,932 | 201,364 to 201,471 | 0, 0 |
| | status, 2 runs | 39,887 to 39,897 (15.5%) | 192,520 to 192,535 | 248,328 to 251,201 | 162, 140 |
| QPSK 9/10 `33m333`, TS replayed | main, 2 runs | 38,840 to 38,994 (17.4 to 17.7%) | 202,714 to 202,716 | 211,795 to 211,934 | none possible |
| | status, 2 runs | 39,389 to 40,147 (15.0 to 16.6%) | 202,825 to 202,829 | 248,064 to 248,264 | none possible |

"Slowest frame" is the run's `encode_cycles_live_max`. The 33.33 Msym/s queue has no dummy path:
an empty queue stops the run, and none did. No run had a late fill, a late switch or a fatal late.

**Every line, not now and then.** The status lines carry the slowest frame of each second. On
8PSK `26m667` their median is 186,953 cycles and their smallest is 174,843, both above main's
slowest frame of the whole run (162,961). The slowest fill of each second has a median of 37,252
cycles, against main's whole-run 36,764. The fill on the 8PSK path, whose control code runs
from the locked instruction cache, rose by up to 3,762 cycles. The 33.33 Msym/s renderer, which
runs from IRAM, rose by 446 at most with synthetic input.

**Not printf.** A probe build (14d205c) wrote the line with plain appends instead of `vsnprintf`
and its float code. On one 30 s TS-replayed 8PSK pace its slowest frame was still 249,449 cycles,
with 60 steady dummies. With synthetic input it was 200,554, with no dummies. The branch went back
to printf (fbf7527, the same source as 937ef5b). Only those summary fields and one status line
per pace were kept for the probe. Its binary reports 937ef5b because CMake was not reconfigured.

**Not yet tested.** The USB Serial/JTAG driver allocates its interrupt without the IRAM flag
(ESP-IDF v6.1 `usb_serial_jtag.c:225`), and its handler sits in flash at 0x4200b620. A line of
about 700 bytes leaves in 64-byte packets, so about 11 transmit interrupts run flash code on core
0, in the middle of the encoder's frames. That fits the pattern, but nothing here measured it.
Two next tests are a 64-byte line, which takes one interrupt, and writing the line straight into
the USB FIFO from the command task with the driver's transmit interrupt off. Each should then
rerun this matrix.

**Memory.** `heap_after_boot` read 7 allocations on both builds, all from `spiinput quad ts
replay`'s setup. No pace added any. `main_stack_min_free` went from 1,448 to 1,368 bytes.

**Forwarding works.** With the probe build flashed, `board.py "dvbpace 14 5m 20 --short"
--monitor 127.0.0.1:1235` sent 30 status records and the final record. A UDP listener got all 31
as type 4 datagrams, with sequence numbers 0 to 30. board.py printed only the final record, and
no line was dropped. During the matrix, board.py's reader also took 40 status lines from every
pace of 937ef5b with `lines_dropped` 0.

## Method

Both builds came from the XIAO overlay (`docs/transmitting.md`), flashed with esptool's
watchdog reset. The run order on each build was: `spiinput off`, then `dvbpace 17 26m667 30` and
`dvbpace 11 33m333 30` three times each. Next came `spiinput quad ts replay`, the same two paces
twice each, then `spiinput off` and `info`. Main ran first, then 937ef5b, then the probe.
`runs.jsonl` holds every record. `tag` is `base`, `status`, `probe-appends` or `monitor-e2e`,
`source` is the commit and `build` is the stamp the firmware reported. Status lines are the rows
with `cmd` `status`. The completion records carry `main_stack_min_free`.
