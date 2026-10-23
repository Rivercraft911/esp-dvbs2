# Wi-Fi or not: same radio state, and the failed frames were a receiver bug

Without Wi-Fi, two 8PSK 8/9 frames failed at 5 Msym/s, and none had failed with Wi-Fi. Both
failures came from a bug in the receiver itself, not from the missing Wi-Fi. At 5 Msym/s the
matched filter starts each batch of symbols from times in 1/32 sample held in 32 bits, which
wrap every 2^27 samples (8.39 s). Both failed frames lost a burst of about 252 symbols that
began within 1.4 samples of a wrap. With the fix (aeb3ea5), 441,550 frames decoded with none
failed.

A register diff found the radio in the same state either way. Of 3,039 PHY, baseband and
analog registers read after the same tuning and gain setup, only one 16-byte table differs, by
one step in every byte, plus one status bit. The table comes from one PLL and temperature
tracking pass that starting Wi-Fi runs once. The analog RF registers, which include the filter
codes, were the same.

Path: transmitter S3 (44:B1:76:B0:E5:54, c3a1c2d, `dvb 2420 16 5m 55 0 128 --short
--phy-solo`) → coax and 30 dB attenuation → receiver S3 (44:B1:76:B0:DA:C8), 2420 MHz,
`rx 2420 32 45 16 5m` checked by `host/rx.py`. Each run is 45 s.

## A/B: Wi-Fi bring-up against none

| Receiver | Runs | Frames exact | Failed | Pilot MER, lowest / median |
|---|---:|---:|---:|---|
| A: Wi-Fi start and stop each run (301e0a9) | 9 | 307,265 | 0 | 23.9 / 29.7 to 29.9 dB |
| B: no Wi-Fi (282835c) | 9 | 305,280 | 1 | 21.9 / 29.4 to 30.0 dB |
| B with the wrap fix (aeb3ea5) | 13 | 441,550 | 0 | 20.3 / 29.8 to 29.9 dB |

A and B were flashed in turn, three runs per block, in the order A B A B A B, so that both saw
the same conditions. Both builds carry a register-snapshot command added for this experiment
(below) that runs only when asked. Both report firmware 282835c because they were built inside
this worktree; their `info` records tell them apart: A has 91,399 bytes of free heap and no
`radio` field, B has 51,482 and `"radio":"ESP_OK"`. `runs.jsonl` labels every record with its
arm.

## The failed frames

| Frame | Run | Pilot MER | Bit errors (none weak) | Data symbols in error | First sample in error |
|---|---|---:|---:|---|---|
| 16,662 | no Wi-Fi, before the arenas (not recorded) | 29.5 dB | 251 | 257 to 508 | 402,653,185 = 3 × 2^27 + 1 |
| 31,590 | B in this A/B | 30.4 dB | 218 | 1,025 to 1,277 | 671,088,641 = 5 × 2^27 + 1 |

The first sample is the frame's header sample plus (90 + symbol) × 3.2 samples. In both frames
the burst is 803 to 806 samples (about 252 symbols, 50 us), and the frame's other symbols are
clean.

**Cause.** At 5 Msym/s the S3 build filters symbols in batches of up to 256
(`demod_mf5_run`). Each batch starts from five symbol centres, and the code notes where the
16-sample window first advances. With 32-bit times in 1/32 sample, a batch whose centres
straddle 2^27 samples sees the base wrap from 2^27 - 16 to 2^32 - 16 and takes that for a window
advance. It then filters the rest of the batch with the wrong coefficient sets. Only batches
that start within about 8 samples of a wrap are hit, about 1% of wraps. That predicts about 0.4
failures in each arm's 40 or so wraps, which fits 0 and 1. The host build filters symbol by symbol
(`demod_mf5_c`), which handles the wrap, and the host's captures are shorter than 8.4 s, so no
host test could see it.

**Fix** (aeb3ea5). The batch plan is now worked out in 64 bits in a portable function
(`demod_mf5_plan`). `tests/test_rx_demod.py` checks that it gives the same plan whatever
multiple of 2^26 samples the time carries, across the wraps at 2^27, 2^28 and 3 × 2^27. The old
32-bit plan fails that check at 576 of the 1,152 times tested. The 1 Msym/s filter wraps only
every 2^30 samples and does not track window advances, so it is unaffected.

## Registers after the same setup

**Method.** An experiment-only `regs MHz gain` command ran a 1 s `rxprobe` and, right after the
capture worker's tuning and gain setup, read these registers:

| Block | Registers read |
|---|---|
| FE2 | 0x60005000, 256 words |
| FE | 0x60006000, 256 words |
| AGC | 0x6001c000, 768 words |
| NRX | 0x6001cc00, 256 words |
| BB | 0x6001d000, 1,024 words |
| SYSCON | 0x60026000, 256 words |
| Dump | 0x60033d00, 64 words |
| Analog I2C | blocks 0x61 to 0x6d, through both hosts |

Snapshots: A twice in one boot and once after a reset, B the same way, and C twice. C is B plus
one `phy_param_track_tot(true, false)` call at boot and a write of 1 to 0x60033d14. All
snapshots are in `regs.jsonl`.

**Result.**
- 69 registers change from one snapshot to the next (RSSI, counters).
- 9 are steady in each bring-up but differ between them:
  - FE 0x60006024 to 0x60006030 (and its mirror at 0x60006224 to 0x60006230): 16 signed bytes,
    each one step lower without Wi-Fi.
  - 0x60033d14: 1 with Wi-Fi, 0 without.
- C matches A in all of them but 0x60033d14, which stays 0 when written, so it is a status bit.
- The analog registers, which include the RX and TX filter codes, are equal.
- With the transmitter off, the noise level was the same: 1.61 to 1.97 LSB rms with Wi-Fi, 1.80
  to 1.93 without.

**PLL tracking.** As the review noted, the PHY's 1 s PLL timer keeps running, and with only the
Bluetooth modem bit set its callback skips `phy_param_track_tot()`. The Wi-Fi bring-up also ran
its captures with only that bit: Wi-Fi's stop clears the Wi-Fi bit. So neither bring-up tracked
during a run. The one difference is the single pass when Wi-Fi's `esp_phy_enable` set the Wi-Fi
bit, which is what moved the FE table. These bytes did not change decoding, and the receiver
does not make that call. It may matter for the transmitter if the table holds TX power, which
was not checked.

## Not settled

**Bursts of dropped frames.** Between 06:37 and 07:00 UTC, 12 of 27 runs lost one
contiguous burst of about 950 frames (1.06 s) each. This happened with every firmware, Wi-Fi or
not, before and after the fix:
- The release task found no free frame slot (`frames_dropped` about 300).
- The demodulator's queue was full (`dropped` about 640).
- No capture unit was lost and the receiver kept lock.

So core 0 stopped consuming frames for about a second. The likely cause is the checker blocking
in its USB record write when the host stops reading: the board's 8 KB USB buffer holds 0.38 s of
records. Four later runs with a copy of `host/rx.py` that logs the gaps between its USB reads
(arm `fixed-gaps`) had no burst and no gap over 62 ms, so the cause is not established. Earlier
sessions had no such burst.

**Correction to [the previous record](../2026-10-23-rx-no-wifi/README.md).** It says ESP-IDF's
heap hooks count every heap call after boot. They count successful allocations and
reallocations, and frees of non-null pointers. Since 282835c the receiver also counts failed
allocations through ESP-IDF's failed-allocation callback; zero-size calls, `free(NULL)` and
direct `multi_heap` calls are still not seen.
