# 8PSK on the board, radio off

8PSK 2/3 and 5/6 come out of the chip bit-exact at 5 and 10 Msym/s, and the 5 Msym/s
transmit schedule holds for 60 s with the encoder running. 10 Msym/s does not fit yet: the
bank fill takes 98k to 112k cycles against an 85,608-cycle deadline. Both fill and encoder
cost more on silicon than the host model predicted.

## What ran

Firmware `3119ce4` (main with exp/dvb-8psk merged), ESP32-S3 at 240 MHz, radio off.
`dvbdump <modcod> <5m|10m> 40 128 1` dumped every DAC sample of 40 continuously encoded
frames over USB; `host/onair.py --rate 40000000 --stream 40 --mer --shift 0` decoded them with
LeanDVB and measured MER against the independent Python encoder (`decodes.jsonl`).
`dvbpace <modcod> <rate> <seconds> [peak] [stall ms]` ran the bank schedule against the cycle
counter with the encoder on core 0 (`runs.jsonl`). Dumps are in ignored
`notes/captures/2026-10-16-8psk-board/`.

## Correctness

| Dump | Frames decoded | LDPC flips | Payload | MER |
|---|---:|---:|---|---:|
| 8PSK 2/3, 5 Msym/s (8 samples/symbol) | 36, from frame 1 | 0 | bit-exact slice | 43.15 dB |
| 8PSK 2/3, 10 Msym/s (4) | 36, from frame 1 | 0 | bit-exact slice | 42.21 dB |
| 8PSK 5/6, 5 Msym/s | 36, from frame 1 | 0 | bit-exact slice | 43.14 dB |
| 8PSK 5/6, 10 Msym/s | 36, from frame 1 | 0 | bit-exact slice | 42.21 dB |

Every dump arrived with matching CRCs, 0 bank-overlap mismatches and 0 underruns. The MERs
match the host model (42.9 dB at 8 and 42.2 dB at 4 samples/symbol, results/2026-10-16-dvb-8psk).

## Timing

| Run | Fills | Late | Longest fill | Smallest slack | Encoder mean per frame | Result |
|---|---:|---:|---:|---:|---:|---|
| `dvbpace 13 5m 60` | 168,209 | 0 | 79,627 | 5,713 | 706k | ESP_OK |
| `dvbpace 15 5m 60` | 168,209 | 0 | 77,918 | 7,419 | 778k | ESP_OK |
| `dvbpace 13 10m 60` | 11 | 2 | 98,321 | | 673k | stopped, ESP_ERR_TIMEOUT |
| `dvbpace 15 10m 60` | 12 | 2 | 112,121 | | 735k | stopped, ESP_ERR_TIMEOUT |

Cycle counts at 240 MHz; one bank is due every 85,608 cycles at 40 MS/s. At 5 Msym/s the
encoder needs 66 to 73% of core 0 (a frame lasts 1,065,312 cycles) and the fill up to 93% of
core 1, so the margin is thin on both cores.

**Where the model went wrong.** The exp/dvb-8psk model predicted fills of 54.2k (8 samples/
symbol) and 73.1k (4). Stalling the encoder separates the two effects (`dvbpace 13 5m 10 128
<stall>`, 10 s each):

| Encoder stall per frame | Frames encoded | Longest fill | Smallest slack |
|---|---:|---:|---:|
| 0 ms | 2,260 | 78,918 | 6,434 |
| 20 ms | 462 | 74,845 | 10,505 |
| 100 ms | 106 | 67,854 | 17,504 |

With the encoder nearly idle the fill still takes 67.9k, 25% over the model; the encoder's
traffic adds up to about 11k more. The encoder itself costs 686k to 706k cycles per 8PSK 2/3
frame in the stream whether or not it is stalled, against 213k in `fecbench pie`
(results/2026-10-16-fec-limit), which keeps its output in internal RAM. The difference is in
the stream's own path (payload generation, the frame written to the PSRAM ring), not in the
FEC stages. Neither cause has been profiled stage by stage yet.

## Not shown

- No RF. The DAC engine's own reads of bank memory were not running.
- 10 Msym/s needs the fill brought under the deadline with margin; 20 Msym/s at 80 MS/s more so.
