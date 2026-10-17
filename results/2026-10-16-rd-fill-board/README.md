# Internal frame slots: 8PSK at 10 Msym/s fits

Keeping the two frames being shaped in internal RAM (two slots, plus a cached copy of the bank
overlap) brings the 8PSK bank fill at 4 samples/symbol from 103.8k to 57.7k cycles and the
encoder from about 676k to 231k cycles per 8PSK 2/3 frame. 8PSK 2/3 and 5/6 at 10 Msym/s now
hold their deadlines with 32% slack, and every output word is unchanged.

Board: ESP32-S3 at 240 MHz, radio off. Baseline firmware `4f3c69f` (profiling only), optimized
firmware `ea8670e`. Records: `baseline.jsonl`, `optimized.jsonl`, `fec.jsonl`, `dumps.jsonl`,
`decode-*.json`; dumps in ignored notes/rd-fill/captures/.

| Run | Firmware | Status | Longest fill | Smallest slack | Late / underruns | Encoder mean per frame |
|---|---|---|---:|---:|---:|---:|
| `dvbpace 13 5m 10` | 4f3c69f | ESP_OK | 79,801 | 5,553 | 0 / 0 | 704,859 |
| `dvbpace 13 10m 10` | 4f3c69f | ESP_ERR_TIMEOUT | 103,840 | | 3 late | 676,167 |
| `dvbpace 13 10m 60` | ea8670e | ESP_OK | 57,708 | 27,633 | 0 / 0 | 231,257 |
| `dvbpace 15 10m 60` | ea8670e | ESP_OK | 57,698 | 27,642 | 0 / 0 | 324,675 |
| `dvbpace 13 5m 60` | ea8670e | ESP_OK | 80,126 | 5,202 | 0 / 0 | 700,500 |

Cycle counts at 240 MHz; a bank is due every 85,608 cycles at 40 MS/s, and a 10 Msym/s
8PSK frame lasts 532,656 cycles, so the encoder takes 43% (2/3) and 61% (5/6) of core 0.
The 15% slack target (longest fill at most 72,766, slack at least 12,842) is met. The 5 Msym/s
path (8 samples/symbol) was not changed by this commit and still has about 6% slack.

Bit-exactness: `fecbench pie` reproduces all 140 reference CRCs. The 16-frame dumps
`dvbdump 13 10m 16 128 1`, `dvbdump 15 10m 16 128 1` and `dvbdump 4 16 128 10` give CRCs
4077039724, 1761772361 and 1729075997, equal to the unoptimized stream's, with 0 overlap
mismatches and 0 underruns. LeanDVB decodes 12, 12 and 14 frames as contiguous bit-exact
slices with 0 LDPC flips; MER 42.22, 42.21 and 43.82 dB, as before.

Cost: internal RAM. The lowest free internal heap during the 10 Msym/s runs was 29.7 KB
(28.3 KB for 5/6), radio off. `fecbench` reports 30,191 bytes free and fails its own 32 KB heap
check, and its worst TS frame (262k cycles) still misses the 250k target, as before. Whether
Wi-Fi start-up fits next to the slots has not been tested: on main, Wi-Fi needed the memory
trims of 81da950 before the first keyed run would start.
