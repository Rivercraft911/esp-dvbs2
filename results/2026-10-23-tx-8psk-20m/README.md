# 8PSK 9/10 at 20 Msym/s

8PSK 9/10 (MODCOD 17) at 20 Msym/s now encodes a frame in 124,101 cycles of 266,328: 53.4%
reserve, was 4.5% (254,301). Three 30 s radio-off paces: no dummy frames, no late fills.
The 8 KiB BCH pair tables it read from flash were the cause; they now sit in internal run
memory (e8ecb9a). The fill on core 1, not the encoder, now limits every 8PSK row at
20 Msym/s: its longest bank fill is 40.7k of 47,220 cycles (13.7% to 13.9% spare).

| `dvbpace`, cycles/frame | Payload | BB | BCH | LDPC | Map | Encoder mean | Reserve | Flash misses/frame |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `17 20m` before (41f6eb9) | 28,672 | 32,972 | 72,373 | 40,829 | 77,808 | 254,280 | 4.5% | 310 |
| `17 20m` after (e8ecb9a) | 28,061 | 3,216 | 44,566 | 24,173 | 22,485 | 124,130 | 53.4% | 0 |
| `17 10m` before (796ec1c) | 28,689 | 32,895 | 72,348 | 40,767 | 77,745 | 254,067 | 52.3% | |
| `17 10m` after (e8ecb9a) | 28,059 | 3,106 | 43,878 | 24,011 | 22,362 | 123,029 | 76.9% | 0 |

10 s paces; the 30 s means are 124,101 (20m) and 122,975 (10m). Board XIAO e5:54, radio off,
xorshift payload, peak 128. "Flash misses" is the EXTMEM data-bus flash miss counter over the
encoder stages (both cores, so an upper bound for core 0).

## Why BB was 33k

2/3 and 5/6 cache their t = 10 byte BCH table in internal RAM; t = 8 read its 8 KiB pair
tables from flash. Added to the other flash tables the encoder walks every frame (PRBS 7.3
KB, Gold 5.4 KB, the LDPC program and the mapper tables), they overran the 32 KiB data cache,
so each stage evicted the next stage's lines: 62 misses in BB, 63 in BCH, 60 in LDPC and 123
in mapping per frame, each costing about 490 cycles (BB's extra 30k over 62 misses). BB itself
does only a 16-byte XOR per 7 instructions; it was waiting on PRBS lines. With the pair tables
internal the encoder takes no flash misses and every stage matches 2/3's per-byte costs.
2/3 and 5/6 at 20m were already miss-free and are unchanged (136.3k and 160.5k).

Cost: 8 KiB more run memory for 8PSK 8/9 and 9/10 at 4 samples per symbol (9/10 20m: 38,320 of
38,672 B low plus 3,072 B high). All 15 pinned 8PSK dumps at 4 samples per symbol (10m and
20m) are exact.

`runs.jsonl` holds the paces (`commit`, `tag` m20-base/m20-pair/m20-30) and the dump checks.
