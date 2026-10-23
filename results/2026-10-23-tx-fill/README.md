# Core-1 bank fill at 80 MS/s: 15% reserve in every mode

**Result.** At 80 MS/s every mode now leaves at least 17.2% of its 47,220-cycle bank deadline
free on core 1. Before, it was 12.9%. The target is 15%: a longest fill of at most 40,137 cycles.
Two changes did it:

- 9880092 times fill phases only in dumps. That saves 0.8-0.9k cycles at the worst bank.
- b96247a turns core 1's FreeRTOS tick off for the run. That saves 0.85-1.3k cycles.

The fill kernels did not change. The core-0 encoder means moved by at most 0.23%.

| Mode | 3b868d5 | + phases in dumps | + tick off | b96247a | Reserve before | Reserve after | Min slack before | Min slack after |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `4 20m` | 36,951 | 36,091 | 35,088 | 35,332 | 21.7% | 25.2% | 9,986 | 11,600 |
| `11 20m` | 36,928 | 36,120 | 35,254 | 35,374 | 21.8% | 25.1% | 10,011 | 11,558 |
| `13 20m` | 40,731 | 39,808 | 38,961 | 38,937 | 13.7% | 17.5% | 6,207 | 7,991 |
| `15 20m` | 40,709 | 39,942 | 39,045 | 39,010 | 13.8% | 17.4% | 6,226 | 7,923 |
| `17 20m` | 40,695 | 39,807 | 38,961 | 38,907 | 13.8% | 17.6% | 6,241 | 8,026 |
| `4 26m667` | 39,253 | 38,418 | 37,419 | 37,642 | 16.9% | 20.3% | 7,687 | 9,290 |
| `11 26m667` | 39,261 | 38,399 | 37,497 | 37,500 | 16.9% | 20.6% | 7,677 | 9,431 |
| `13 26m667` | 37,532 | 37,731 | 36,450 | 36,419 | 20.5% | 22.9% | 9,401 | 10,403 |
| `15 26m667` | 37,667 | 37,623 | 36,631 | 36,676 | 20.2% | 22.3% | 9,268 | 10,256 |
| `17 26m667` | 37,546 | 37,489 | 36,472 | 36,446 | 20.5% | 22.8% | 9,387 | 10,378 |
| `4 33m333` | 41,119 | 40,336 | 39,032 | 39,098 | 12.9% | 17.2% | 5,822 | 7,834 |
| `11 33m333` | 40,959 | 39,995 | 38,953 | 38,988 | 13.3% | 17.4% | 5,971 | 7,944 |

How to read the table:

- The cycle columns give the longest single bank fill (`max_fill_cycles`) from two 10 s
  `dvbpace` runs of each mode. The value shown is the larger of the two. The runs agreed
  within 500 cycles.
- Reserve is 1 - fill / 47,220.
- Min slack is the closest any fill came to its deadline in either run (`min_fill_slack_cycles`).
  It also counts time spent between fills.
- The modes are QPSK 1/2 and 9/10 (MODCOD 4 and 11) and 8PSK 2/3, 5/6 and 9/10 (13, 15 and
  17). They run at 20, 26.67 and 33.33 Msym/s. These are all the modes that use an 80 MS/s DAC.
- No run had a late fill, late switch, underrun or dummy frame. All returned `ESP_OK`.

Board: XIAO ESP32-S3 44:B1:76:B0:E5:54, radio off, Wi-Fi never started, peak 128.

The four columns are series in `runs.jsonl`, tagged by `tag`:

- `before1`/`before2` (3b868d5).
- `phases1`/`phases2`: the 9880092 fill code, built as `3b868d5-dirty` before the commit.
- `tick1`/`tick2`: the b96247a fill code, built as `9880092-dirty`. That build re-enabled the
  tick right after the run instead of after the RF cleanup.
- `after1`/`after2`: clean b96247a, with a board reset before each mode.

The middle columns isolate each change: phases first, then the tick. The 8PSK `26m667` queue
fill already skipped phase marks in paces, so 9880092 does not change it.

## Where a worst fill went

The worst-call split from the 3b868d5 runs comes from the per-phase marks that 9880092 now
keeps for dumps only. "Kernel" is the shaping call. "Copy" is the ring window read.
"Notify" wakes the encoder on core 0. "Other" is what no phase covers.

| Mode | Fill | Copy | Kernel | Save | Notify | Other |
|---|---:|---:|---:|---:|---:|---:|
| `4 33m333` | 41,119 | 493 | 36,323 | 477 | 737 | 3,089 |
| `13 20m` | 40,731 | 2,980 | 31,838 | 477 | 2,382 | 3,054 |
| `4 26m667` | 39,253 | 3,625 | 29,279 | 477 | 2,403 | 3,469 |

The two changes remove most of "Other": 0.8-0.9k for the marks and 0.85-1.3k for the tick.
What remains is 1.0-2.1k a fill. It is not split further. It is likely function entry, the
second call at the physical bank wrap and the bookkeeping after the kernel, but that is an
estimate.

At `26m667` QPSK, about 10k of the 39.3k cycles is outside the kernel:

| Part | Cycles | Status |
|---|---:|---|
| Ring window read | 3.6k | Measured |
| Encoder wake | 2.4k | Measured |
| Overlap save | 0.5k | Measured |
| Phase marks | 0.8k | Measured, by difference |
| Core-1 tick | 1.0k | Measured, by difference |
| Unattributed | 1.6k | Not split |

The kernel itself runs above its instruction count. At 33.33 Msym/s it took 36.3k cycles for
15,740 samples, which is 2.3 cycles a sample. Its inner loop, `machine_axis_render_pie`, has
374 instructions per 192-sample run, which is 1.95 a sample or 30.7k for the bank. The
measured kernel is 18% above that, and the measured span also includes the page bookkeeping
in `moon_shape`. The fill code is in IRAM (`FILL_TEXT`), so flash instruction misses cannot
add to it.

The next levers, not needed for 15%:

- The encoder wake, `xTaskNotifyGive` across cores, takes up to 2.4k on the banks that
  release a slot.
- The ring window read takes 3-3.9k at 20m and 26m667.

## Core 1's tick

The radio worker is the only task on core 1 during a run. On this dual-core build, core 1's
1 kHz FreeRTOS tick does no timekeeping (core 0 counts ticks). It only checks time slicing.
It still interrupts about one fill in five, at roughly 1k cycles each time.

b96247a clears core 1's systimer alarm interrupt enable (alarm 1, `SYSTIMER_ALARM_OS_TICK_CORE0
+ 1`) for `paced()` and `stream()`. It sets the enable again after the RF cleanup, so the
transmitter is already off by then.

The interrupt watchdog checks core 1 through its tick hook (CONFIG_ESP_INT_WDT_CHECK_CPU1,
800 ms timeout). `schedule()` therefore calls `esp_vApplicationTickHook()` every 1,024 banks.
That is at most 365 ms at 40 MS/s and 201 ms at 80 MS/s. The host model in
`tests/test_radio_duration.py` checks this spacing over 540 s at both rates.

When the enable comes back, IDF's handler (`freertos/port_systick.c`) runs every skipped tick
at once. Measured with a temporary print around the re-enable:

| Run | Catch-up |
|---|---:|
| 10 s pace | 6.9M cycles (29 ms) |
| 60 s pace | 24.1M cycles (100 ms) |

The 50 extra seconds cost 17.2M cycles, which is 344 cycles per skipped tick. That
extrapolates to about 0.8 s after a 540 s run. The time is spent on core 1 only, after the
run.

## Checks

- Every `tests/test_*.py` passes. tx (XIAO) and rx build.
- Host tests changed with the commits:
  - `test_8psk_memory` expects paces without phase marks.
  - `test_radio_duration` checks the tick-hook cadence.
- All 95 pinned dumps match their CRCs on 43f5286, the branch head, whose fill code equals
  b96247a. The board CRC, the host CRC and the golden CRC agree, with zero overlap
  mismatches (`dumpcheck` rows in `runs.jsonl`).
- These are radio-off paces. A keyed run also shares SRAM with the DAC's reads, so the keyed
  confirmation belongs to a keyed table.

## Correction

`results/2026-10-23-tx-8psk/README.md` calls MODCOD 4 "QPSK 1/4" in its replay section.
MODCOD 4 is QPSK 1/2. That record is left as it was run.
