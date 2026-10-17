# Build-out and rd-encoder together, radio off

The integrated build (`exp/next`: main + exp/buildout + exp/rd-encoder) produces byte-identical
transmit samples to the build-out, and every paced run holds its deadlines. Of rd-encoder's BCH
candidates, the byte-table kernel wins on the bench but not in the live stream, so the
transmitter uses the register-nibble kernel in every mode. That takes 8PSK 5/6 at 10 Msym/s from
376k to 291k encoder cycles per frame. No RF was transmitted.

ESP32-S3 at 240 MHz. Firmware `503b26a` for `fecbench`, dumps and `pace-byte.jsonl`;
`503b26a` plus the one-line flag change committed as `b237d35` for `pace-nibble.jsonl`
(the build stamp reads `503b26a`).

## Encoder variants, `fecbench` (one run each)

Every variant: 140 of 140 output CRCs equal the Python reference, 14 PIE-against-C checks clean
(`fec-<variant>.jsonl`, `python3 tests/test_fecbench.py <file> --correctness-only`).
Median cycles per normal frame, GCS input:

| Mode | legacy | nibble | pie (byte BCH for t = 10, 12) | sram |
|---|---:|---:|---:|---:|
| 8PSK 2/3 | 229.2k | 206.3k | 206.4k | 209.2k |
| 8PSK 5/6 | 252.5k | 230.3k | 324.3k | 327.5k |
| 8PSK 9/10 | 189.1k | 146.8k | 146.8k | 146.8k |
| QPSK 1/2 | 697.5k | 681.2k | 731.1k | 734.6k |

Byte BCH meets its own target only for 8PSK 2/3 (73% of legacy BCH). For 5/6 the LDPC and mapping
stages slow down next to it (31k to 46k and 49k to 78k), and QPSK 1/2 BCH gets slower, not faster.
Register-nibble BCH improves every mode. The sparse mapping table saves 7-9% of mapping time,
short of its 15% target, with no regression.

## In the live stream, `dvbpace` (30 s, CW ID on)

| Run | BCH kernel | Encoder mean / max | Longest fill | Smallest slack | Late, underruns |
|---|---|---:|---:|---:|---|
| 8PSK 5/6, 10 Msym/s | byte | 385.8k / 521.5k | 56,876 | 28,460 | 0, 0 |
| 8PSK 5/6, 10 Msym/s | nibble | **290.9k** / 579.5k | 56,929 | 28,410 | 0, 0 |
| 8PSK 2/3, 10 Msym/s | byte | 257.2k / 439.8k | 56,899 | 28,437 | 0, 0 |
| 8PSK 2/3, 10 Msym/s | nibble | 262.1k / 365.2k | 56,881 | 28,460 | 0, 0 |
| 8PSK 2/3, 5 Msym/s | nibble | 262.1k / 332.6k | 43,017 | 42,318 | 0, 0 |
| QPSK 1/2, 1 Msym/s | nibble | 685.2k / 742.2k | 41,387 | 43,948 | 0, 0 |

The stage profile shows why byte BCH does not carry over. For 8PSK 2/3 it cuts BCH from 99k to
76k cycles per frame, but BB framing rises from 8k to 25k and PL framing from 3k to 10k, with
three to six times the flash cache misses. The encoder's tables live in flash and its workspace
in PSRAM, and both share the 32 KiB data cache; the 8 KiB byte table pushes the others out.
The single 579k frame for 5/6 (others 289k-291k) left the encoder at least 29,569 symbols ahead,
more than a frame, so it cost nothing. Build-out reference: 5/6 at 376k, 2/3 at 257k.

## Output

The five dumps of `notes/buildout/dumps2.sh` (QPSK 1/2; 8PSK 2/3 and 5/6 at 5 and 10 Msym/s)
have the same CRCs as `results/2026-10-16-buildout`, which LeanDVB decoded bit-exact
(`dumps.jsonl`). The BCH kernel choice changes no output bit.

## Not shown

- No RF. The keyed in-box runs follow.
- Moving the encoder workspace or its hottest tables into internal RAM, or a 64 KiB data cache,
  should remove the cache conflict; neither is measured.
