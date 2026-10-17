# Four-symbol 3 samples/symbol kernel on the board, radio off

`block4_pie` shapes a whole 16,380-word bank at 3 samples/symbol in **46,443 cycles**
(2.835 cycles/sample), against the host model's 46,427: the model is within 0.03%. All block
checks match the C path (check 2897995758). It beats rd-short's measured 3 sps kernel (66.5k)
but still exceeds the 42,804-cycle bank at 80 MS/s, and its tables take 72 KiB of internal RAM,
which the transmitter does not have. 26.67 Msym/s stays out of reach with this design.

Firmware `4f10a77`, ESP32-S3 at 240 MHz, radio off. Records: `mem.jsonl`, `fec.jsonl`,
`pace.jsonl`, `dumps.jsonl`.

| Check | Result |
|---|---|
| `block4_c` / `block4_pie` | 279,918 / 46,443 cycles per bank, same check |
| Table build, each of three | 7.52 M cycles (31 ms, once at start) |
| `fecbench nibble` | 140 of 140 CRCs, 14 PIE checks |
| `dvbpace 13 10m 30` (unchanged stream) | ESP_OK, 0 late, fill 57,215, slack 28,120 |
| Five unchanged dumps | main's CRCs, 0 overlap mismatches |

`tests/test_maxrate.py --membench` stops on the first record, "internal allocation failed",
which main's `membench` also logs for its first memory test (results/2026-10-16-integrate);
the block records after it are complete. Whole-bank cost only: with overlap reuse the stream
would shape about 14,268 new words per bank, which the model puts near 43.1k.
