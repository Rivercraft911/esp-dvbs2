# 5 Msym/s encoder fix and the 20 Msym/s short pulse, radio off

exp/next4 (main `32ea843` + exp/max-encoder `0ec9964` + exp/max-astra `ba549b4`) is faster or
equal to main in every mode and every output CRC is unchanged. The new `20ms6` mode (20 Msym/s,
6-symbol pulse) fills a bank in 40.5k of 47,220 cycles, against 45.6k for `20m`, and leaves
34.9 KB of internal RAM free with Wi-Fi up instead of 21.2 KB. No RF was transmitted.

Firmware `a2c845b`, ESP32-S3 at 240 MHz. Records: `runs.jsonl` (paced runs and dump summaries;
the first `dvbpace 13 5m 30` is from `host/next4-board.sh`, the rest from a rerun after that
script stopped at it), `heap.jsonl` (`wifimem`: Wi-Fi start and stop, never keyed).

| `dvbpace`, 30 s | Longest fill | Smallest slack | Encoder mean / live max | main encoder |
|---|---:|---:|---:|---:|
| 8PSK 2/3, 5 Msym/s | 38,652 | 46,683 | 228,652 / 238,603 | 298,327 |
| 8PSK 5/6, 5 Msym/s | 38,492 | 46,845 | 265,635 / 275,202 | 421,593 |
| 8PSK 2/3, 10 Msym/s | 44,349 | 40,987 | 167,519 / 168,955 | 167,113 |
| 8PSK 2/3, 20 Msym/s (`20m`) | 45,571 | 1,379 | 167,447 / 170,222 | 167,031 |
| 8PSK 2/3, 20 Msym/s (`20ms6`) | 40,508 | 6,440 | 169,057 / 171,545 | new |
| 8PSK 5/6, 20 Msym/s (`20ms6`) | 40,546 | 6,402 | 187,147 / 188,347 | new |
| QPSK 1/2, 1 Msym/s | 42,135 | 42,989 | 681,921 / 685,896 | 682,159 |

Every run `ESP_OK` with zero late fills, late switches and underruns. Free internal RAM with
Wi-Fi up: 34,875 B for `20ms6` and 21,579 B for 8PSK 9/10 at 5 Msym/s, the tightest mode.

Correctness: `fecbench pie` 140 of 140; all ten dumps match their CRCs, including the `20ms6`
pair (3178964763, 4256019779), which decoded bit-exact in LeanDVB at 45.0 dB MER on exp/max-astra.

The script's own gates for 5 Msym/s, set from exp/max-encoder measured alone (222.8k), were
missed by 0.6% (mean) and 1.9% (live max) after the merge. The 20ms6 fill misses the 15% margin
gate (40,137) by 0.9%. Both are reported as misses; neither affects a deadline.
