# Faster fill and encoder together, radio off

With exp/max-fill and exp/max-encoder merged, 8PSK at 10 Msym/s needs 43.8k cycles per bank
(main: 57.2k) and 167k cycles per 8PSK 2/3 frame (main: 262k), and every output bit is
unchanged. Against the 80 MS/s targets, the encoder now fits a 20 Msym/s frame (266,328 cycles)
and even a 26.67 Msym/s one (199,746); the fill is 2% over the 42,804-cycle bank. Internal RAM
is now the tight resource: 21.1 KB free with Wi-Fi up in the worst mode. The 5 Msym/s encoder
got slower and is being fixed. No RF was transmitted.

Firmware `0759644` (main `ea0958d` + exp/max-fill `5860c48` + exp/max-encoder `603833c`),
ESP32-S3 at 240 MHz, board in the closed shielded box. Records: `runs.jsonl`, `fec.jsonl`,
`decodes.jsonl`. Each branch was also run alone on the board with the same results for its own
part (notes on the branches).

## What changed

- **Fill (max-fill):** finished samples in the bank overlap are reused instead of re-shaped,
  the symbol cache shrinks from 554 to 10 bytes (no more byte-wise ROM `memmove`), and phase
  scaling moves into the PIE unpack. +10.3 KB internal RAM.
- **Encoder (max-encoder):** the mapper writes packed symbols directly, fixed headers and
  pilots are prepared once, and at 4 samples/symbol the encoder workspace and BCH table move
  from PSRAM and flash into internal RAM. Flash misses per frame fall to zero there. +21.2 KB
  internal RAM at 4 samples/symbol.

## Correctness

`fecbench pie`: 140 of 140 CRCs, 14 clean PIE checks. The five dumps have main's CRCs
(QPSK 1435641601; 8PSK 2/3 and 5/6 at 5 and 10 Msym/s: 1937985440, 1163322913, 474263962,
1757765864) with zero overlap mismatches, and LeanDVB decodes each bit-exact: 36-38 frames,
zero LDPC flips and BCH failures, MER 43.82 (QPSK), 43.78 (5 Msym/s) and 40.66-40.67 dB
(10 Msym/s), as on main.

## Timing, `dvbpace`, 30 s each

| Run | Longest fill | Smallest slack | Encoder mean / live max | Min lead, symbols | main: fill / encoder |
|---|---:|---:|---:|---:|---|
| 8PSK 2/3, 10 Msym/s | 43,780 | 41,559 | 167,113 / 167,967 | 33,712 | 56,881 / 262,085 |
| 8PSK 5/6, 10 Msym/s | 43,791 | 41,543 | 185,036 / 185,809 | 33,679 | 56,929 / 290,916 |
| 8PSK 2/3, 5 Msym/s | 37,400 | 47,938 | 298,327 / 306,340 | 15,051 | 43,017 / 262,118 |
| 8PSK 5/6, 5 Msym/s | 37,454 | 47,884 | 421,593 / 439,250 | 11,486 | 42,845 / 376,092 |
| QPSK 1/2, 1 Msym/s | 42,184 | 43,154 | 682,159 / 687,830 | 228,634 | 41,387 / 685,170 |

Every run: `ESP_OK`, 0 late fills, 0 fatal, 0 underruns. "Live max" leaves out only the frames
encoded before playback starts. The main column is results/2026-10-16-next and -buildout.

At 5 Msym/s the encoder is slower than main (5/6: 422k against 376k-386k). There the
workspace stays in PSRAM, and the new mapping path takes about twice the flash misses in BB
framing and three times in mapping. It uses 40% of core 0 at that rate, so deadlines are not at
risk, but it is a regression and a fix is in progress on exp/max-encoder.

## RAM with Wi-Fi up, `wifimem`

`wifimem` starts and stops Wi-Fi exactly as `dvb` does and never keys.

| Mode | Free internal, Wi-Fi up | Largest block | main |
|---|---:|---:|---:|
| 8PSK 2/3, 10 Msym/s | 21,211 B | 10,752 B | 54,499 B |
| 8PSK 5/6, 10 Msym/s | 21,691 B | 10,752 B | 53,155 B |
| 8PSK 2/3, 5 Msym/s | 25,467 B | 15,872 B | 34,371 B |
| 8PSK 9/10, 5 Msym/s | 21,079 B | 7,680 B | 32.7 KB (build-out probe) |
| QPSK 1/2 | 53,135 B | 31,744 B | 63,991 B |

All stay above the 20 KB floor used for keyed runs, the worst by 1.1 KB. The next RAM-hungry
change needs RAM found first; the 6-symbol pulse's half-size table at 8 samples/symbol is the
obvious source.

## Not shown

- No RF; keyed in-box runs on this build follow.
- 80 MS/s: these runs use the 40 MS/s schedule at 4 and 8 samples/symbol. The fill at 80 MS/s
  must also cope with the DAC engine reading twice as fast.
- `membench` again logged "internal allocation failed" for its first memory test, as main did
  in results/2026-10-16-integrate; all 32 shaper checks ran and match.
