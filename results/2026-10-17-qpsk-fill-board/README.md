# QPSK fill at 3 and 4 samples/symbol on the board, radio off

The new QPSK fill kernels produce the host model's samples exactly and keep their speed. At
3 samples/symbol (26.67 Msym/s at 80 MS/s) a whole bank takes 34,584 cycles of the 47,220
available (26% slack); at 4 (20 Msym/s) 32,247. Every dump decodes bit-exact in LeanDVB. These
paced runs time the fill alone (`encoder_timed:false`, pre-encoded input in internal RAM); the
complete transmitter with the encoder running at 26.67 Msym/s is the next step.

Firmware `e50e794`, ESP32-S3 at 240 MHz. Records: `mem.jsonl`, `dumps.jsonl`, `pace.jsonl`,
`decodes.jsonl`. The first board run (`e975eb2`, notes) had wrong samples: the workspace came
from plain `malloc` at a 12-byte offset, but the PIE loads need 16-byte alignment. QEMU with the
same offset reproduced every bad sample; `e50e794` allocates it aligned and tests for it.

| Dump, 16 frames | CRC32 (= host model) | LeanDVB | MER, raw samples |
|---|---:|---|---:|
| QPSK 1/2, 26.67 Msym/s | 3388792897 | 14 frames, 0 errors | 37.99 dB |
| QPSK 9/10, 26.67 Msym/s | 1271511239 | 14 frames, 0 errors | 37.98 dB |
| QPSK 1/2, 20 Msym/s | 3789273049 | 14 frames, 0 errors | 41.22 dB |
| QPSK 9/10, 20 Msym/s | 2081653259 | 14 frames, 0 errors | 41.21 dB |

The pulse pre-compensates the DAC's hold, which a digital dump lacks; with an ideal hold the
host model gives 45.02 dB. `membench`: all four QPSK kernel checks match Python.

| `dvbpace`, 30 s, fill only | Longest fill | Smallest slack | Late |
|---|---:|---:|---:|
| `qpsk3` (26.67 Msym/s) | 34,584 | 12,364 | 0 |
| `qpsk4` (20 Msym/s) | 32,247 | 14,703 | 0 |
