# QPSK at 26.67 Msym/s in real time, radio off

The lofty QPSK goal holds on silicon: QPSK 9/10 at 26.67 Msym/s, 46.6 Mb/s in 36 MHz, with the
encoder, ring and fill running together at 80 MS/s. Every bank was on time for 30 s with 16.6%
of the bank period to spare (longest fill 39,139 of 47,220 cycles), and the dumps decode
bit-exact. Internal RAM is back above the floor in every mode probed. No RF was transmitted.

Firmware `10ee312` (exp/qpsk-goal: main `2cf2ac2` + exp/qpsk-fill + the live 3 samples/symbol
stream), ESP32-S3 at 240 MHz. Records: `runs.jsonl` (paced runs), `dumps.jsonl`, `heap.jsonl`
(`wifimem`: Wi-Fi start and stop, never keyed). A slim check, per the 5-minute default.

| `dvbpace`, 30 s | Longest fill | Bank period | Smallest slack | Encoder mean / live max | Min lead, symbols |
|---|---:|---:|---:|---:|---:|
| QPSK 9/10, 26.67 Msym/s (`26m667`) | 39,139 | 47,220 | 7,816 | 227,692 / 228,865 | 39,258 |
| QPSK 1/2, 26.67 Msym/s | 39,005 | 47,220 | 7,948 | 198,324 / 200,078 | 40,318 |
| QPSK 9/10, 20 Msym/s (`20mq4`) | 36,766 | 47,220 | 10,187 | 227,880 / 228,829 | 46,080 |
| 8PSK 9/10, 5 Msym/s | 38,870 | 85,608 | 46,469 | 215,215 / 221,246 | 16,835 |

Every run `ESP_OK`, zero late fills, late switches and underruns. Gates set before the run:
longest fill at most 40,137 and slack at least 7,083 cycles (15%): both 26.67 Msym/s runs pass.
One core's budget per QPSK frame at 26.67 Msym/s is 299,538 cycles; the encoder uses 76%.

| Dump, 16 frames | CRC32 (= host model) | LeanDVB | MER, raw samples |
|---|---:|---|---:|
| QPSK 1/2, 26.67 Msym/s | 3388792897 | 14 frames, 0 errors | 37.99 dB |
| QPSK 9/10, 26.67 Msym/s | 1271511239 | 14 frames, 0 errors | 37.98 dB |
| QPSK 1/2, 20 Msym/s (`20mq4`) | 3789273049 | 14 frames, 0 errors | 41.22 dB |
| QPSK 9/10, 20 Msym/s (`20mq4`) | 2081653259 | 14 frames, 0 errors | 41.21 dB |

Raw-sample MER leaves out the DAC hold that the pulse pre-compensates (45.0 dB with an ideal
hold in the host model). Regression dumps `13 20ms6` (3178964763) and `11 20m` (2779934507)
match. Free internal RAM with Wi-Fi up: 41,387 B for QPSK 26.67 Msym/s and 22,351 B for 8PSK
9/10 at 5 Msym/s (19,043 B on next5): the 3,328 B recovered here put it back above 20 KB.

Not shown: RF. 36 MHz is wider than the +-10 MHz measured for the transmit filter in this
bring-up mode, so the wide filter mode is needed before this signal is clean on air.
