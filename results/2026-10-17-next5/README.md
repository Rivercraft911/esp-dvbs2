# QPSK at 5, 10 and 20 Msym/s on main, radio off

exp/next5 (main `4f926aa` + exp/qpsk-enc `8437931`) passes 55 of 56 board checks. QPSK now runs
at 5, 10 and 20 Msym/s (MODCOD 4-11) with an encoder of 156k-185k cycles per frame, and all
eighteen dump CRCs match: main's ten and the eight QPSK ones. The one failure is internal RAM:
8PSK 9/10 at 5 Msym/s leaves 19,043 B free with Wi-Fi up, below the 20,000 B floor (predicted
19,019 B; the QPSK code costs every mode 2,560 B of instruction RAM). Wi-Fi starts normally
(`ESP_OK`); the floor is a margin, and recovering those bytes is part of the next step. No RF.

Firmware `298235f`, ESP32-S3 at 240 MHz. `board.log` is the full output of `host/next5-board.sh`
(fecbench, eighteen dumps, six 30 s paced runs, three `wifimem` probes that start and stop
Wi-Fi without keying).

| `dvbpace`, 30 s | Longest fill | Bank period | Smallest slack | Encoder mean / live max |
|---|---:|---:|---:|---:|
| 8PSK 2/3, 5 Msym/s | 38,017 | 85,608 | 46,429 | 215,228 / 221,949 |
| 8PSK 2/3, 20 Msym/s (`20m`) | 45,550 | 47,220 | 1,404 | 167,798 / 171,096 |
| 8PSK 2/3, 20 Msym/s (`20ms6`) | 40,492 | 47,220 | 6,458 | 169,296 / 171,068 |
| QPSK 1/2, 20 Msym/s | 45,986 | 47,220 | 962 | 156,529 / 159,164 |
| QPSK 9/10, 20 Msym/s (34.9 Mb/s) | 45,944 | 47,220 | 1,007 | 184,782 / 185,384 |
| QPSK 1/2, 1 Msym/s | 41,565 | 85,608 | 43,299 | 682,449 / 688,475 |

Every run `ESP_OK`, zero late fills, late switches and underruns. QPSK at 20 Msym/s uses the
existing 9-symbol fill with 2% slack; the measured QPSK kernels (exp/qpsk-fill, 32.2k at
4 samples/symbol) are not in this build yet.

| `wifimem` | Free with Wi-Fi up | Floor |
|---|---:|---:|
| QPSK MODCOD 8, 10 Msym/s | 31,879 B (min 29,955) | pass |
| 8PSK 2/3, `20ms6` | 34,187 B | pass |
| 8PSK 9/10, 5 Msym/s | **19,043 B** (largest block 7,680) | **fail, 20,000** |
