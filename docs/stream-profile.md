# Radio-off stream profiles

`dvbpace` and `dvbdump` report where each core spends its cycles. Neither starts the radio.
Timing includes interrupts and the instrumentation itself. No counters are read per symbol.

The final result's `profile.encoder` has payload generation, BB framing/scrambling, BCH,
LDPC, mapping and PL framing. Each stage reports calls, total cycles, maximum cycles, data
accesses and instruction/flash/PSRAM cache misses. Divide cycles by calls for cycles/frame.
Payload generation is included in `encode_cycles_mean`, but excluded from `fecbench` timing.
Startup frames are included in both the stream's total and its stage profiles.
`layout` records table, context, workspace, payload and slot addresses for placement checks.

`profile.fill` separates symbol copying/ring management, whole-symbol shaping (including
the C tail), and partial-symbol shaping/copying. `fill_words` includes every callback, so
`stage.cycles * 16380 / fill_words` gives cycles per equivalent bank. `worst_call` holds the
components of the **same** slowest callback. A physical bank wrap makes two callbacks;
`max_fill_cycles` from the scheduler is the whole-bank maximum. Do not add independent
phase maxima to estimate it. Remaining cycles include profiling, notification and glue.

Both cores share the caches ([ESP32-S3 TRM v1.8](https://documentation.espressif.com/esp32-s3_technical_reference_manual_en.pdf),
section 4.3.3.2, p405). Cache observation windows overlap between cores and include interrupts.
They are requests/misses, not stall cycles, bytes or exclusive per-core counts. Internal SRAM
bus contention is not counted. EXTMEM register offsets B4..D8 and their meanings come from
ESP-IDF v6.1 `components/soc/esp32s3/register/soc/extmem_reg.h:675`. The command temporarily
sets the counter address filters to the TRM table 4.3-2 apertures, then restores them. It
does not clear counters or change cache policy. Subtractions permit one counter wrap per
stage. Validate nonzero activity on silicon before interpreting a zero as no misses.

```sh
python3 host/board.py "dvbpace 13 10m 60" --timeout 120 --out results/<new-run>/runs.jsonl
python3 host/stream_profile.py results/<new-run>/runs.jsonl
```

The analyzer normalizes fill phases and encoder stages, prints the shared-cache windows,
and checks the 60 s timing target: status `ESP_OK`, zero late fills, fatal deadlines and
underruns, maximum fill <= 72,766 cycles and minimum slack >= 12,842 cycles. At 240 MHz and
40 MS/s, the period is `14268 samples * 240 MHz / 40 MS/s = 85608 cycles`; these limits
leave at least 15%. A dump tests correctness, not paced deadlines. RF remains unmeasured.

## Internal frames at 4 samples/symbol

Only the 4-sps 8PSK path uses two internal frame slots. The 8-sps and QPSK paths retain
eight PSRAM slots. All encoder APIs and ring bytes remain canonical phases 0..7.

The 4-sps table uses `512 windows * 16 words/window * 4 B/word = 32768 B`. Its reserved
store is 65,536 B. The unused upper half holds one 22,194-byte frame, the 3,330-byte dummy,
and payload if it fits, with 16-byte starts. The second frame is a separate internal heap
allocation made before the fast context. MODCOD 17's 7,264-byte payload exceeds the remaining
7,216 B by 48 B, so it gets an internal heap allocation. Allocation failure is explicit.

A 552-byte internal cache retains overlap/history across calls. The maximum repeated span
is the ordinary 2,112-sample bank overlap plus the first bank's extra 64 samples of lead;
`8 history symbols + ceil(2176 samples / 4 samples/symbol) = 552 symbols`. A short second
piece at the physical bank wrap must preserve the preceding piece's overlap. Slots are
released only below the cache's oldest symbol. Subsequent reads use that cache, so the
encoder can overwrite released slots safely. A task notification wakes the encoder after
each release instead of waiting for a 1 ms polling tick.
The notification function is placed in IRAM with `stream.lf`; a link assertion checks its
address. The valid notification path's critical-section and cross-core-yield callees are
already in IRAM in IDF v6.1. The rest of FreeRTOS keeps its default placement.

`tests/test_ring.py` checks separate slots, overlap, order, starvation, two threads and a
bank-schedule model through 80% encoder load with two slots. `tests/test_stream.py` runs
the actual stream lifecycle and compares every DAC word with the plain encoder, checks
dump CRCs, repeated commands, notifications and allocation failures. Both run in CI.
These tests establish software correctness; they establish no silicon speed or RF result.
