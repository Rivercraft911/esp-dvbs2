# Bank geometry, switch timing and the two-core schedule at 80 MS/s

At 80 MS/s a bank switch needs far less overlap than the 2,048 samples inherited from
esp-sdr-zephyr, but the overlap was never the main cost. The recorded late switches (49,228 of
15.8 million) match, in rate and size, the 1 kHz FreeRTOS tick on core 1 landing just before
the old 320-cycle interrupt mask; masking the whole wait before a switch removes that cause. With lead 128 and
tail 512 at 80 MS/s, a bank is due every 47,220 cycles instead of 42,804 (+10.3%). On main's
fill (results/2026-10-16-next2) that turns 20 Msym/s from a worst bank 1.0k over its deadline,
held only by the tail's allowance, into one with about 2.5k (5%) to spare (model). The CW ID now
keeps 20 WPM at 80 MS/s, so `dvb` runs there. The Wi-Fi MAC interrupt lands on core 0, not on
the fill.

Labels: **measured** (board records in `results/`), **derived** (arithmetic from measured
numbers or the code), **model** (an estimate that silicon must confirm), **unknown**.

## The bank schedule

The DAC engine replays `BANK_WORDS` = 16,380 words of the selected bank and keeps its offset when
the CPU switches banks (esp-sdr-zephyr 3214c16, `doc/undocumented-registers-esp32s3.md` lines
282-284 and 368-372). Sample n lives at word n mod 16,380 in both banks. Bank k plays samples
[k·STEP, (k+1)·STEP) and also holds LEAD samples before and TAIL samples after them, so

    LEAD + STEP + TAIL = 16,380

and every fill rewrites a whole bank while the other one plays. A fill is due every STEP
samples. The switch for handoff h = k·STEP is aimed LEAD/2 samples early. Let d be the DAC's
read position lag behind the CPU's cycle-counter model (engine start latency minus any
read-ahead), in samples, and λ the delay of the switch past its aim, in cycles. The DAC plays
the right samples if

    early side:  LEAD/2 + d <= LEAD,  so  d <= LEAD/2
    late side:   λ/CPS - LEAD/2 - d < TAIL

with CPS = 240 MHz / DAC rate = 6 at 40 MS/s and 3 at 80 MS/s. The code's fatal limit keeps LEAD
of the tail back for an unknown read-ahead: it stops the run once a switch is (TAIL - LEAD)·CPS
cycles past the nominal handoff. Its `late_switches` and `max_switch_late_cycles` count from the
handoff too, not from the aim.

**Why lead 64 and tail 2048.** Both come from esp-sdr-zephyr's transmitter
(`lib/esp_sdr/src/esp_sdr_tx_dac.c` lines 296-297), whose leader sleeps on a 10 kHz kernel tick
and "can end up to two ticks late", so it wakes 300 µs early (line 475), and which also saw USB
and IPI interrupts stretch its fills (lines 361-363). Nothing in this project measured a need
for 2,048. The tail is also the fill's elastic allowance: a fill may overrun its deadline by up
to the fatal limit and the next fill catches up. For an isolated slow bank the total budget is
(16,380 - 2·LEAD)·CPS whatever the tail; a shorter tail moves cycles from that allowance into
every bank's deadline.

## What the recorded lateness is

**Measured**, all runs in `results/` that did not abort:

| Runs | Switches | Late | Late per second | Latest, cycles |
|---|---:|---:|---:|---:|
| paced (radio off), 7 result sets | 14,217,788 | 44,288 | 8.71-8.77 | 1,339 (rd-fill-board), 755 otherwise |
| keyed (`tx`, `dvb`), 3 sets | 1,575,516 | 4,940 | 8.67-8.85 | 763 |

0.31% of switches are late in every set, at the same rate with Wi-Fi up and the radio keyed.

**Derived:** before this branch, interrupts were masked only for the last 128 + 192 = 320 cycles
before a 40 MS/s switch. An interrupt of D cycles that starts in the W = D - 320 cycles before
the mask delays the switch by up to W. With a tick at 1 kHz and 2,803.5 switches per second,

    late per second = 1000 · W · 2803.5 / 240e6

gives 8.74/s for W = 748 cycles, the measured rate, and lateness uniform up to W matches the
recorded maxima of 731-763 cycles. So the cause is core 1's FreeRTOS tick (`CONFIG_FREERTOS_HZ`
1000), an interrupt service of about 1,068 cycles (4.5 µs, inferred, not timed directly). One
event of 1,339 cycles in 15.8 million switches came from an interrupt of at least 1,659 cycles
(cause unknown). Nothing else interrupts core 1 often enough to show in these statistics, keyed
or not.

**What the switch needs at 80 MS/s.** With the old mask, the window shrinks to 96 + 128 = 224
cycles and the worst recorded event becomes 1,659 - 224 = 1,435 cycles, 479 samples. With a
safety factor of 2 that needs (TAIL - LEAD) >= 958 samples: tail 1,024 with lead 64, a 45,876-cycle
deadline (+3,072).

**What this branch does instead** (`radio_s3.c`, `schedule`): it masks interrupt levels 1-3 from
the end of the fill to the switch. A maskable interrupt can then only run inside a fill, and a
switch is late only if its fill overran. Cost (derived): an interrupt that would have run in
the idle wait moves into the next fill. The tick period (240,000 cycles) is longer than a bank
(47,220 or 85,608 cycles), so the worst bank still carries at most one tick; the average is
unchanged. Core 1's interrupt latency grows to at most one bank's slack (357 µs at 40 MS/s),
far inside the 800 ms interrupt watchdog (`CONFIG_ESP_INT_WDT_TIMEOUT_MS`). Levels 4-5 and the
NMI stay enabled (TRM v1.8 section 9.3.2, table 9.3-2, p. 544-545; `XCHAL_EXCM_LEVEL` 3 in IDF
v6.1 `core-isa.h`); none fire on core 1 in normal operation. **Prediction:** `late_switches` 0
in every run whose fills are on time.

What the CPU-side numbers cannot show: the time from the `BANK_USAGE` store to the engine
acting on it, and d. On the ESP32-C5, esp-sdr-zephyr saw CPU loops that store to a PHY
register stall for 3 to 36 µs with interrupts locked, cause unknown (`doc/hardware-quirks.md`
line 325); nobody has looked for that on the S3. The tone-phase capture in
results/2026-10-16-radio (0.5 s, about 1,400 switches) shows no dropped or repeated sample at
40 MS/s, which bounds d to the 32-sample early margin there but says nothing about rare stalls.
The `gap` probe below measures both.

## 80 MS/s geometry

DAC_CTRL (0x60033d64) bit 15, `DUMP_DAC_RATE_80M`, selects 80 MS/s (register document lines
263-275, measured there). The branch sets it from the run's geometry in both writes that start
the engine. What else changes at 80 MS/s:

- CPS halves to 3, so the same geometry halves every deadline and halves LEAD/2 in time.
- The engine reads its own bank at 320 MB/s instead of 160.
- The pulse tables do not change: at the same samples per symbol the sample-hold correction is
  the same in normalised frequency, so `20m` plays exactly the `10m` samples and `10m80` the `5m`
  samples (dump CRCs equal, below).
- The CW ID's timing is in samples. Unscaled it would play at 40 WPM, above the 20 WPM limit
  for an automatic CW ID (47 CFR 97.119(b)(1)). `station_id_init_rate` doubles every duration
  at 80 MS/s (dot, 5 ms edge, 10 µs envelope step, 540 s repeat), so the ID stays 20 WPM. Its
  40-sample tone row then plays at +2 MHz instead of +1; a +1 MHz tone would need an 80-word
  row, 160 bytes more internal RAM per run and a new copy kernel. At 10 kS/s the 80 MS/s ID is
  sample for sample the 40 MS/s one: `iddump N0CALL 80` must give the same CRC, 1965033598.
- `radio_signals.c` tones are 40 MS/s tables. The transmit filter was measured at about ±10 MHz
  in this bring-up mode; 20 Msym/s needs ±13.5 MHz. esp-sdr-zephyr reports about 4 dB better MER
  at 80 than at 40 MS/s.

The chosen 80 MS/s geometry is **lead 128, tail 512** (`radio_s3.h`):

| Lead | Tail | Step | Deadline, cycles | vs 42,804 | Fatal limit | Late tolerance | Early tolerance |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 64 | 2048 | 14,268 | 42,804 | | 5,952 | 26.0 µs | 0.40 µs |
| 64 | 1024 | 15,292 | 45,876 | +3,072 | 2,880 | 13.2 µs | 0.40 µs |
| **128** | **512** | **15,740** | **47,220** | **+4,416** | 1,152 | 7.2 µs | 0.80 µs |
| 128 | 256 | 15,996 | 47,988 | +5,184 | 384 | 4.0 µs | 0.80 µs |
| 64 | 256 | 16,060 | 48,180 | +5,376 | 576 | 3.6 µs | 0.40 µs |

Late tolerance is (TAIL + LEAD/2) samples at d = 0, early tolerance LEAD/2. Lead 128 keeps the
0.8 µs early margin that 40 MS/s has shown on air, because d is unmeasured and may be a fixed
time rather than a fixed number of samples. Tail 512 gives 7.2 µs, above the latest switch ever
recorded: with lead 128 the old mask window is 192 + 128 = 320 cycles again, so that interrupt
would land a switch 1,339 cycles (446 samples, 6.4 µs from the aim) late, inside the tail even if
the new mask somehow failed. The run's fatal limit (1,152 cycles) would stop it first. Each further 256 tail words is 768 cycles per bank. Step 15,740 is a multiple of 4, so
whole-symbol bank starts stay 16-byte aligned; 15,740 mod 8 = 4 cuts 8-sps symbols at bank
edges exactly as 14,268 does, which the overlap cache's two spare symbols cover (the ring model
fails without them, `tests/test_ring.py`).

**What a smaller overlap gives back per bank (model).** The deadline gains 3 cycles per word,
4,416 cycles here. The fill still writes all 16,380 words, so it does not get cheaper, but it
writes 10.3% fewer banks per second. Main's fill (exp/max-fill) now copies the re-read overlap
from a CPU-side cache of finished samples instead of shaping it again, at about 0.95 cycles per
word (next2 profile: 2,060 cycles to save 2,176 words) against 2.35 for shaping. At 80 MS/s each
bank re-reads 640 words instead of 2,112 and keeps 768 instead of 2,176 (`dvb.cached`), so the
copies cost about 2.7k cycles less per bank, while the bank shapes 1,472 more words (+3.5k).

My first version of this note said copying the overlap could not beat recomputing it, from a
2.5 cycles/word memcpy estimate. That was wrong: max-fill's 128-bit copies from a CPU-side cache
cost 0.95, and they never read the engine's bank.

## Predicted fill against the deadline

Per-bank work at 80 MS/s equals the 40 MS/s work at the same samples per symbol, apart from the
geometry. Model inputs: main's measured profile (results/2026-10-16-next2, `dvbpace` 30 s at
40 MS/s), scaled per phase: shaping by words shaped, re-read and save copies at 0.95 cycles per
word, the rest by symbols; the worst bank keeps its measured excess over the mean (a tick and an
encoder notification). Script: `notes/next2_model.py` in this worktree.

| Mode | Measured at 40 MS/s, mean / worst | Model at 80 MS/s, mean / worst | Deadline | Minimum slack |
|---|---|---|---:|---:|
| `20m` on 64/2048 | 40.2k / 43.8k | 40.2k / 43.8k | 42,804 | -1.0k, held by the 5,952 allowance |
| `20m` on 128/512 | | 41.2k / 44.7k | 47,220 | 2.5k (5.3%) |
| `10m80` on 128/512 | 33.1k / 37.4k | 33.2k / 37.6k | 47,220 | 9.7k (20.5%) |

What changed since the first version (build-out fill, 48.0k / 57.2k at 4 sps): `20m` went from
failing by 9.3k to holding by 2.5k, and `10m80` from 4.6k to 9.7k of slack. 20 Msym/s still
misses the 15% margin; that needs a worst bank of at most 40.1k.

## The two-core schedule

**Where interrupts land (derived from the IDF v6.1 binaries and config, consistent with the
measurements).** `esp_wifi_start()` sends its work to the Wi-Fi task through `ieee80211_ioctl`,
which posts with `pp_post` whenever the caller is not the Wi-Fi task (`libnet80211.a`,
`ieee80211_ioctl.o`). There `_do_wifi_start` calls `wifi_hw_start`, whose
`ic_set_interrupt_handler` (`libpp.a`, `if_hwctrl.o`) routes interrupt sources 2 and 0 (Wi-Fi
power and MAC) to CPU line 0 of the core in `g_wifi_menuconfig` offset 52. That field is copied
from `wifi_init_config_t.wifi_task_core_id` (offset 116), which `WIFI_INIT_CONFIG_DEFAULT` sets
from `CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0=y`. The handler `wDev_ProcessFiq` is installed and
line 0 enabled on the calling core, the Wi-Fi task's core 0. So the MAC interrupt runs on the
encoder's core even though the radio worker calls `esp_wifi_init` from core 1. The esp_timer
interrupt and task and the USB console are on core 0 too. The keyed runs' late-switch rate
equals the radio-off rate, so Wi-Fi adds nothing measurable to core 1. On core 0 the mean
encoder time per frame is the same keyed and radio off on the same build (261,261 against
262,085 cycles at 10 Msym/s, results/2026-10-16-keyed-10m and -next), but the slowest keyed frame
at 5 Msym/s took 507k against 333k radio off, so Wi-Fi may cost core 0 an occasional burst.

**Core 1's own costs (measured or derived):** the tick, about 1,068 cycles per ms (0.44%, at
most one per bank); the fill's notification of the encoder, up to 2,348 cycles once per frame
(docs/stream-profile.md). The fill can mask levels 1-3 for a whole bank without harm, but that
only moves the tick; removing it would mean stopping core 1's tick for the run, which needs
`CONFIG_ESP_INT_WDT_CHECK_CPU1=n`. Not done: 0.44% for a system-wide change.

**Spare time (model, cycles per second at 240 MHz per core), on main's fill and encoder
(next2: 167k / 185k cycles per 2/3 and 5/6 frame at 4 sps; 298k / 422k at 8 sps, workspace in
PSRAM):**

| Mode, geometry | Fill, core 1 | Encoder 2/3 and 5/6, core 0 |
|---|---:|---:|
| 20 Msym/s, 128/512 | 209 M (87%) | 151 M (63%), 167 M (69%) |
| 10 Msym/s at 80 MS/s, 128/512 | 169 M (70%) | 134 M (56%), 190 M (79%) |

On the build-out code both cores were full at 20 Msym/s. Now core 0 has the spare time:
about 70-90 M cycles per second at 20 Msym/s against core 1's 31 M. Encoder work cannot move
into core 1's slack; if anything, fill work could move to core 0. At 10 Msym/s on 80 MS/s the
5/6 encoder uses 79% of core 0 with only two packed frames in the ring (the 8-sps layout),
which the ring model says leaves little lead (458 symbols at 80% load).

**Engine read traffic (model).** The banks are SRAM1 Block6 (0x3FCC_0000) and Block7
(0x3FCD_0000) (TRM v1.8 table 15.3-3, p. 685), and SRAM1 is built from separate 8 and 16 KB
sub-blocks (section 4.3.2, p. 404). The engine reads only the bank it owns and the CPU may not
touch that bank, so the fill's stores and loads go to other blocks. The TRM does not document
the engine or its arbitration. Measured at 160 MB/s: keyed fills matched radio-off fills within
0.4% (results/2026-10-16-keyed-10m). Prediction at 320 MB/s: no measurable effect; checked by
keyed test K1 below.

## Code on this branch

- `radio_s3.c` `schedule`: interrupts masked from fill end to switch.
- `radio_s3.h`, `radio_s3.c`: per-rate geometry and DAC_CTRL bit 15; `radio_s3_run_rate`,
  `radio_s3_paced_run_rate`, `radio_s3_dry_run_rate`. Unknown rates fail before the worker.
- `station_id`: `station_id_init_rate`; durations scale with the rate. `iddump [CALL] [40|80]`.
- `main.c`, `dvbtx.c`: `dvb`, `dvbpace` and `dvbdump` take `20m` (4 sps) and `10m80` (8 sps) at
  80 MS/s; `tx` takes an optional `40|80` after the gain; records gain `dac_rate`; main's sample
  cache keeps only what the run's geometry re-reads.
- `tx <MHz> gap <s> [gain] [40|80]` and `capture.py --signal gap`: the switch probe.
- Tests: the host stream harness gets the five published board CRCs on both geometries; the ring
  model runs the packed ring on the 80 MS/s geometry; a QEMU app runs the real dry run at both
  geometries (2.03 million samples checked); the probe's silence rule is compiled from
  `radio_s3.c`; the 80 MS/s ID decodes at 20 WPM and its 10 kS/s dump has the 40 MS/s CRC; the
  `iddump`, `dvb` and `tx` parsers and the keyed entry at both rates.

Cost (`idf.py size` against main `aa75bab`): internal RAM 84 bytes less (code), static data
unchanged, flash code +832 bytes. A run's ID context grows by 4 bytes of heap.

## Rejected

- Tail 256 now: 768 more cycles per bank, but its 4.0 µs late tolerance is below the latest
  switch ever recorded if the mask failed. Revisit after the `gap` probe.
- Lead 64 at 80 MS/s: 192 cycles per bank more, but 0.4 µs of early margin against an unmeasured
  d.
- A third bank: 64 KiB of internal RAM.
- A +1 MHz ID tone at 80 MS/s: an 80-word row costs 160 bytes of internal RAM per run and a new
  copy kernel, for no regulatory gain; the +2 MHz tone sits inside the occupied band.
- Stopping core 1's tick, or notifying the encoder only when it waits: below 1% each, the latter
  saves only the cheap case. Not worth the complexity before the kernel work lands.

## Board commands, radio off, in this order

Pass criteria are set before the run. Each command takes about a minute or less.

1. `fecbench pie`: 140 of 140 CRCs, 14 clean PIE checks.
2. `iddump N0CALL` and `iddump N0CALL 80`: both CRC 1965033598, 208,800 binary bytes,
   `dac_samples` 208,800,000 and 417,600,000, `tone_hz` 1000000 and 2000000, 0 overlap
   mismatches. `morse.py <dump> --rate 10000 --tone 0 --expect N0CALL` matches for both.
3. `dvbdump 4 40`, `dvbdump 13 5m 40 128 1`, `dvbdump 15 5m 40 128 1`, `dvbdump 13 10m 40 128 1`,
   `dvbdump 15 10m 40 128 1`: CRCs 1435641601, 1937985440, 1163322913, 474263962, 1757765864;
   0 overlap mismatches.
4. `dvbdump 13 20m 40 128 1`, `dvbdump 15 20m 40 128 1`, `dvbdump 13 10m80 40 128 1`: CRCs
   474263962, 1757765864, 1937985440 (the host harness gets these), 0 overlap mismatches,
   `sample_rate` 80000000.
5. `dvbpace 13 10m 30`, `dvbpace 15 10m 30`, `dvbpace 13 5m 30`, `dvbpace 4 30`: `ESP_OK`;
   late fills, fatal and underruns 0; **`late_switches` 0** (next2: about 350); longest fill
   within 1.5k cycles of next2 (43.8k, 43.8k, 37.4k, 42.2k).
6. `dvbpace 13 20m 30`, `dvbpace 15 20m 30`: `ESP_OK`, `dac_rate` 80000000, `step_words` 15740;
   late fills, fatal, late switches and underruns 0; longest fill at most 46,000 and smallest
   slack at least 1,200 cycles (model 44.7k and 2.5k).
7. `dvbpace 13 10m80 30`: as 6, longest fill at most 40,000 (model 37.6k). Underruns expected
   0 for 2/3; `15 10m80` may underrun (two-slot ring at 79% encoder load), which is not a
   schedule fault.

## Keyed tests for the coordinator, inside the shielded box

Not run by this branch. Callsign stored; cabled through at least 30 dB where a receiver is used.

- **K0, 40 MS/s, with the receiver:** `tx 2420 gap 30 0` (the start and end IDs take about
  10.4 s), two 0.5 s captures at 61.44 MS/s between the IDs, about 6 s and 24 s into the run, then
  `capture.py <file> --format i16 --rate 61440000 --scale 2048 --signal gap` on each. It
  measures where the DAC really switches. Pass: every gap 1 to 63 samples (inside the 64-sample
  lead) and `off_grid` 0 apart from ID spaces; `lag_samples_median` (d) equal in both captures
  within 2 samples (no clock drift). d at or below 16 samples (0.4 µs) would allow lead 64 at
  80 MS/s; p99 below 32 samples and no long gaps would allow tail 256.
- **K1, 80 MS/s, no receiver needed:** `dvb 2420 13 10m80 30 0 128`, then `dvb 2420 13 20m 30 0 128`
  and `15 20m`. Pass: as the matching `dvbpace` (6 and 7), plus `fe_gain_index` 0, `dac_rate`
  80000000, longest fill within 2% of the radio-off run (the engine's 320 MB/s reads), and
  free internal RAM with Wi-Fi up at least 20 KB (next2: 21.1 KB worst).
- **K2, 80 MS/s, with the receiver:** `tx 2420 gap 30 0 80` (+2 MHz tone), captured and analysed as
  K0 with `--dac-rate 80e6`. Pass: every gap 1 to 127 samples, the two captures' d equal within 2
  samples. Also capture the start ID of K1's `10m80` run and decode it with
  `morse.py --tone 2000000` (plus the measured carrier offset): N0CALL at 20 WPM.

## Next

The `gap` probe at 40 MS/s (K0). It is the only measurement that turns lead and tail from
inherited numbers into measured ones, and it runs on today's keyed path.
