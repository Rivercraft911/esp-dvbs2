# DVB-S2 pulse shaping into a DAC bank, PIE vector kernels

Firmware `7a8544f` (branch exp/shaper merged with main), ESP32-S3 at 240 MHz. `python3 host/board.py
membench`, three runs, records in `runs.jsonl`; `python3 host/shaper.py verify` compares every
board checksum with the Python model of the same tables and symbols. All 18 shaper records match.

The pulse is a root-raised cosine (roll-off 0.20) cut to 9 symbols with a Kaiser window. It is
split into three windows of three symbols; each sample is the sum of three 32-bit table words,
and one XOR turns the sum into the DAC word (I in bits 9:0, Q in bits 19:10). The 8PSK table at
8 samples/symbol is 64 KiB. Host model: Annex A mask margin +0.23 dB with the DAC's hold, MER
38.7 dB from the table (notes, not measured on air).

| Kernel | Cycles/sample | One 16380-sample bank | Predicted |
|---|---:|---:|---:|
| 8PSK, 8 samples/symbol (5 Msym/s at 40 MS/s), C | 14.97 | 1022 µs | about 16 |
| 8PSK, 8 samples/symbol, PIE | **1.776** | **121 µs** | 1.78 |
| QPSK, 40 samples/symbol (1 Msym/s at 40 MS/s), C | 10.58 | 722 µs | about 11 |
| QPSK, 40 samples/symbol, PIE | **1.754** | **120 µs** | 1.73–1.78 |

Cycles per sample include fetching symbols, updating the history and computing table addresses.
The prediction assumed one cycle per instruction plus load-use stalls and single-cycle 128-bit
SRAM access; the measurements confirm it.

At 40 MS/s a bank is due every 356.7 µs, so 8PSK shaping uses 34% of one core. At 80 MS/s the
same work would be 59% of one core. These runs had the radio off; contention with the DAC engine
reading the other bank is not included.
