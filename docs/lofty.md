# 8PSK at 80/3 Msym/s

Core 0 encodes normal DVB-S2 frames and publishes compact four-symbol descriptors.
Core 1 renders them into the DAC banks at 80 MS/s, using a five-symbol pulse with
roll-off 0.35. MODCODs 13, 15 and 17 reached 51.6, 64.6 and 69.8 Mb/s in
[30 s keyed tests](measurements.md#8psk-at-36-mhz), firmware eee5153.
The dumped samples decode bit-exact; RF reception is still unmeasured.

## Queue and memory

The existing shaping store holds the entire table, 54 pages and the stream mux:

```
36864 table bytes + 54 * 528 page bytes + 160 mux bytes = 65536 bytes
```

Each page holds 128 four-symbol descriptors, a prefetch guard and page metadata.
The contiguous queue occupies internal SRAM before the reserved DAC banks at
`0x3fcc0000` through `0x3fce0000`. Records include its actual addresses and SRAM
levels, using ESP-IDF v6.1 `heap/port/esp32s3/memory_layout.c`'s region numbering.
The store is shared with other modes' tables and rings, never an extra allocation.

Encoder tables are reserved before the context and 8,144-byte FEC workspace so
smaller allocations cannot fragment the largest required block. BCH, PRBS, Gold
and mapper data stay internal; the cold cache tail, extra BCH table, dummy
descriptors and prepared input use PSRAM. These allocations exist only in
8PSK `26m667`. After Wi-Fi starts, the stream selects 54 resident pages, checks
20,000 B minimum free internal RAM and 2,048 B extra current reserve, then fills
the queue before playback. It makes no post-Wi-Fi queue allocation.

A frame uses 44 pages. The [Wi-Fi-off page sweep](measurements.md#8psk-at-36-mhz)
found that ten spare pages eliminated steady dummies at MODCOD 17. Mean encoder
time alone did not predict this: mapping publishes in bursts. Payload, BB, BCH
and LDPC for the next frame already run before waiting for pages; only mapping
and PL publication block. Scratch is shared, so the following FEC cannot
overwrite an incompletely published mapped frame.

## Admission and dummies

The fill never waits for the encoder. At a PLFRAME boundary it admits a complete
frame, or a partial frame covered by the production bound; otherwise it plays a
standard 3,330-symbol dummy PLFRAME, EN 302 307-1 §5.5.1. Dummy pairs retain
four-symbol descriptor rounding and vector alignment. Boundary history is
spliced into the same renderer; no data frame or input byte is skipped.

A normal frame has 22,194 symbols: 66,582 samples at three samples/symbol, or
199,746 CPU cycles at 240 MHz and 80 MS/s. It alternates between 43 full pages
plus a 534-sample tail, and a 1,530-sample prefix, 42 full pages and a 540-sample
tail. Both need 44 pages. A full page spans 1,536 samples.

Let G be the largest recurring publication gap measured after the initial mapping
burst, excluding full-queue sleep. The planning bound is W = 2G cycles/page.
Let P be ready pages, R = 9 reserve pages, and D = 0 while mapping or 47,220 cycles
while awaiting a bank-end wake. For every missing page k = 1 through 44-P:

```
D + k*W <= (P-R+k-1)*2944        [CPU cycles]
```

The renderer issues at least 128*23 = 2,944 instructions per page. This lower
bound uses its executed instruction trace and the S3's single-issue rule
(TRM v1.8 §1.7, Table 1.7-2). The linear inequality needs only its two endpoints
checked. Thresholds update after EOF publication, outside the measured gaps.
Acquire/release counters transfer page ownership; the producer arms its wait
before rechecking space, and the consumer wakes it once per completed bank.

The bound is conditional on measured production and wake latency, not arbitrary
CPU starvation. Its nine pages reserve at least 4,606 symbols while current-frame
production is unfinished; following-frame FEC can reduce lead after EOF.
Records report startup/steady/alignment dummies, actual lead and bound misses.

## Optional memory and checks

The interrupt observer allocates its ISR state internally only when requested;
its report and the deferred queue trace use PSRAM. Normal runs allocate neither.
Profiling helpers stay out of line in IRAM instead of being repeated in each
fill branch. The vector renderer remains in IRAM. The small 8PSK control section
is flash-backed, loaded and locked in instruction cache only for `26m667`, and
unlocked on cleanup. It occupies at most one 4 KiB way of the configured 16 KiB
cache. Setup repeats after Wi-Fi initialization; `renderer_icache_bytes` reports
its size. Other modes do not load or lock it. Firmware eee5153 passed keyed
checks at all three rates and the full older-mode self-check. See the
[measured reserves](measurements.md#8psk-at-36-mhz).

Without a board:

```sh
python3 tests/test_lofty.py notes/lofty-native
python3 tests/test_lofty_handoff.py notes/lofty-handoff
python3 tests/test_pace_pages.py notes/lofty-pages
python3 tests/test_lofty_memory.py --map firmware/build/esp_dvbs2.map
```

These check every DAC word against Python, unchanged golden CRCs, forced dummy
transitions, queue wraps, allocation failure cleanup, and code residency. Native
clocks and heaps are synthetic. The [PIE fixture](../tests/lofty/README.md) checks
the target kernels. The board coordinator is radio-off, with Wi-Fi up:

```sh
caffeinate -i python3 host/jitter.py notes/lofty-queue-check --queue-gate
```

It checks 13/15/17 for 20 s each: ESP_OK, zero late fills, longest fill at most
40,137 cycles and at least 20,000 B minimum free internal RAM, while reporting
steady dummies. The profiled pace in eee5153 fails this diagnostic gate; the
keyed runs meet their fill deadlines with no steady dummies. Use the [self-check](../examples/self-check/README.md) for older
modes and the unchanged `wifimem 17 5m` floor of 22,351 B.
