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

## Internal 8PSK frames

Every 8PSK rate keeps its frames in internal RAM, so neither core touches PSRAM per symbol.
QPSK keeps its eight raw PSRAM slots (233 ms of encoder lead at 1 Msym/s), allocated in the
same order as before, because its slowest fill is a PSRAM copy whose cost depends on which
data cache sets the slots share with the encoder's workspace.

Frames are packed two phases per byte: byte i holds phase 2i in its low nibble and phase
2i + 1 in its high nibble (dvbs2.h). A frame then takes 22,194 / 2 = 11,097 bytes instead of
22,194. The encoder writes packed frames without a full-frame phase buffer
(`dvbs2_fast_map_packed`): each 1440-symbol run between pilots is mapped by the existing PIE
kernel into scratch after the index pass, packed by `dvbs2_pie_nibbles` (VUNZIP.8 splits even
and odd phases, the odd ones move up four bits, OR) and copied out. Header and pilots pack
the same way. Every part of a PLFRAME starts at an even symbol, so parts never share a byte.

Each packed part is 16-byte aligned with 16 spare bytes after it, because the unpack reads
whole vectors: `packed_bytes(n) = (n / 2 + 31) & ~15`, 11,120 B per frame and 1,696 B for
the 3,330-symbol dummy.

| Rate | Table | Frames | Store's upper half | Internal heap for the ring |
|---|---:|---:|---|---:|
| 10 Msym/s (4 sps) | 32,768 B | 3 | frames 0 and 1, dummy, payload: 31,200 B at most (MODCOD 17) | 11,120 B (frame 2) |
| 5 Msym/s (8 sps) | 65,536 B | 2 | none (the table fills the store) | 29,308 to 31,200 B, one block |

At 10 Msym/s the 5/6 encoder needs about 71% of a 532,656-cycle frame period. The ring model
(`tests/test_ring.py`) gives two frames as little as 244 symbols of lead at 70 to 80% load and
underruns at 90%; three frames keep 18,871 symbols at 95%. At 5 Msym/s the encoder needs at
most 36% and two frames keep 11,213 symbols in the model.

The fill unpacks the symbols it needs into its window (`ring_unpack`). When every symbol
lands at an address of its own parity, `shaper_unpack_pie` turns 16 packed bytes into 32
phases in about 11 cycles (AND, shift, AND, VZIP.8); odd heads and tails stay scalar. The
fill shifts its window by the first symbol's parity to make that true: the history is eight
symbols, and for sps = 4 or 8 the parity of `first / sps` is bit log2(sps) of `first`.

An overlap cache keeps `8 + 2 + ceil(2176 / sps)` symbols: 554 at 4 sps and 282 at 8 sps. 2,176
samples is the bank overlap (2,112) plus the first bank's extra lead (64). The extra two
symbols cover a symbol cut at each end of a bank; 16380 % 8 = 4, so 8-sps banks cut symbols.
The host ring model fails without them at 8 sps. Slots are released only below the cache's
oldest symbol. A task notification wakes the encoder after each release instead of waiting
for a 1 ms polling tick. It costs the fill core up to 2,348 cycles (board, notification max),
so only the packed rings use it; QPSK polls as before. The notification function is placed in
IRAM with `stream.lf`, and a link assertion checks its address.

`tests/test_ring.py` checks raw and packed slots, overlap, order, starvation, two threads and
the bank-schedule model at 4, 8 and 40 sps. `tests/test_stream.py` runs the actual stream
lifecycle for eight modes and compares every DAC word with the plain encoder, checks the ring
layout, dump CRCs, repeated commands, notifications and every allocation failure.
`tests/test_dvbs2_c.py` compares 60 packed frames with the model; `tests/tx_samples.c` checks
the PIE packer and unpacker against C in 13,058 cases on the host and in QEMU, and `fecbench
pie` compares PIE and C packed frames on the board. These establish correctness, not speed.

## Profile cost

Reading the four EXTMEM counters at each fill-phase mark added 0.9k to 1.6k cycles to the
slowest bank of a paced run, which `dvb` does not have. `dvbpace` therefore marks fill phases
with CCOUNT only (`"fill_counters": false`, cache fields 0); `dvbdump`, which is not timed,
keeps them. Encoder stages always read them.

## Wi-Fi start-up memory

`wifimem [<modcod> [5m|10m]]` allocates a mode's transmitter memory as `dvb` would, then runs
`radio_s3_wifi_probe`: the radio task's set-up (station ID, worker stack) and Wi-Fi start-up
(init, NULL mode, start with the PHY calibration, power save off, promiscuous receive, channel
1), then stop and deinit. It never retunes, keys, sets gain, starts a tone, grants a bank or
starts the DAC; NULL mode sends no frames. It needs a stored callsign. Every `dvb` record now
carries the same `wifi_*` heap fields from the real start-up.
