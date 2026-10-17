# Bank geometry, switch timing and the two-core schedule at 80 MS/s

At 80 MS/s a bank switch needs far less overlap than the 2,048 samples inherited from
esp-sdr-zephyr, but the overlap was never the main cost. The recorded late switches (49,228 of
15.8 million) match, in rate and size, the 1 kHz FreeRTOS tick on core 1 landing just before
the old 320-cycle interrupt mask; masking the whole wait before a switch removes that cause. With lead 128 and
tail 512 at 80 MS/s, a bank is due every 47,220 cycles instead of 42,804 (+10.3%). That is not
enough on its own: 20 Msym/s needs about 48.0k cycles per bank on average (57.2k worst) on
today's fill, and the encoder already uses 98% of core 0. Core 1 has no spare time to take
encoder work at 80 MS/s. The Wi-Fi MAC interrupt lands on core 0, not on the fill.

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
- The CW ID is generated at 40 MS/s (`STATION_RATE`, a 40-sample tone row, Morse timing in
  samples). At 80 MS/s it would play at +2 MHz and 40 WPM, above the 20 WPM limit for an
  automatic CW ID. Keyed runs therefore stay at 40 MS/s on this branch; radio-off paced runs
  insert the same ID samples, which take half the air time.
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
writes 10.3% fewer banks per second. The overlap cache shrinks from 554 to 202 symbols at 4 sps
(282 to 106 at 8), and each bank re-reads 168 instead of 536 cached symbols: about 1.07 kB less
copying per bank at 4 sps, about 0.67k cycles at the measured 384 MB/s memcpy rate (0.34k at
8 sps). Copying overlap samples instead of recomputing them would not help: the source bank is
owned by the engine during the fill, and memcpy costs about 2.5 cycles per word, the kernel's
own rate.

## Predicted fill against the deadline

Per-bank work at 80 MS/s equals the 40 MS/s work at the same samples per symbol, so the measured
build-out profile carries over (results/2026-10-16-buildout, 60 s runs).

| Mode | Measured per bank (mean / worst) | Model at 80 MS/s, this branch | Deadline | Verdict |
|---|---|---|---:|---|
| `20m`, 20 Msym/s, 4 sps | 48.0k / 57.2k | 47.3k / 56.5k | 47,220 | fails: worst bank 9.3k over, fatal after the first slow bank |
| `10m80`, 10 Msym/s, 8 sps | 36.7k / 42.9k | 36.4k / 42.6k | 47,220 | holds, about 4.6k (10%) minimum slack |

The fill engineer's targets for 20 Msym/s at this geometry, with 15% margin: worst bank at most
40.1k, or 41.1k if the 1,152-cycle allowance is counted.

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

**Spare time (model, cycles per second at 240 MHz per core):**

| Mode, geometry | Fill, core 1 | Encoder 2/3, core 0 | Both cores |
|---|---:|---:|---:|
| 20 Msym/s, 64/2048 | 269 M (112%) | 236 M (98%) | 105% |
| 20 Msym/s, 128/512 | 244 M (102%) | 236 M (98%) | 100% |
| 10 Msym/s at 80 MS/s, 128/512 | 187 M (78%) | 118 M (49%) | 63% |

At 20 Msym/s neither core has time to give, so moving LDPC, PL framing or payload work into
core 1's slack is not possible there, and splitting the fill across cores is not possible either.
The slack that exists today (a third of each bank at 10 Msym/s, 40 MS/s) is not needed at that
rate. The factor of two has to come from less work per sample and per frame.

**Engine read traffic (model).** The banks are SRAM1 Block6 (0x3FCC_0000) and Block7
(0x3FCD_0000) (TRM v1.8 table 15.3-3, p. 685), and SRAM1 is built from separate 8 and 16 KB
sub-blocks (section 4.3.2, p. 404). The engine reads only the bank it owns and the CPU may not
touch that bank, so the fill's stores and loads go to other blocks. The TRM does not document
the engine or its arbitration. Measured at 160 MB/s: keyed fills matched radio-off fills within
0.4% (results/2026-10-16-keyed-10m). Prediction at 320 MB/s: no measurable effect; checked by
keyed test K1 below.

## Code on this branch

- `radio_s3.c` `schedule`: interrupts masked from fill end to switch.
- `radio_s3.h`, `radio_s3.c`: per-rate geometry, `radio_s3_paced_run_rate` and
  `radio_s3_dry_run_rate`; keyed runs stay at 40 MS/s.
- `main.c`, `dvbtx.c`: `dvbpace` and `dvbdump` take `20m` (4 sps) and `10m80` (8 sps) at
  80 MS/s; records gain `dac_rate`; the overlap cache follows the run's geometry.
- `tx <MHz> gap <s>` and `capture.py --signal gap`: the switch probe.
- Tests: the host stream harness dumps every 4- and 8-sps mode on both geometries with equal
  CRCs; the ring model runs the packed ring on the 80 MS/s geometry; a QEMU app runs the real
  dry run at both geometries (2.03 million samples checked); the probe's silence rule is compiled
  from `radio_s3.c` and checked for every switch position in the overlap.

Cost (`idf.py size`): internal RAM +76 bytes of code against main (mask -32, probe +108), static
data unchanged; flash code +440 bytes. The overlap cache keeps its 554-byte array and uses 202
bytes of it at 80 MS/s.

## Rejected

- Tail 256 now: 768 more cycles per bank, but its 4.0 µs late tolerance is below the latest
  switch ever recorded if the mask failed. Revisit after the `gap` probe.
- Lead 64 at 80 MS/s: 192 cycles per bank more, but 0.4 µs of early margin against an unmeasured
  d.
- A third bank: 64 KiB of internal RAM.
- Copying overlap samples instead of recomputing them: see above.
- Stopping core 1's tick, or notifying the encoder only when it waits: below 1% each, the latter
  saves only the cheap case. Not worth the complexity before the kernel work lands.

## Board commands, radio off, in this order

Pass criteria are set before the run. Each command takes about a minute or less.

1. `fecbench pie`: all 140 CRCs as before (this branch does not touch the encoder).
2. `dvbdump 4 40`, `dvbdump 13 5m 40 128 1`, `dvbdump 15 5m 40 128 1`, `dvbdump 13 10m 40 128 1`,
   `dvbdump 15 10m 40 128 1`: CRCs 1435641601, 1937985440, 1163322913, 474263962, 1757765864;
   `overlap_mismatches` 0.
3. `dvbdump 13 20m 40 128 1`, `dvbdump 15 20m 40 128 1`: CRCs 474263962 and 1757765864 (the
   10m samples on the other geometry), `overlap_mismatches` 0, `sample_rate` 80000000.
   `dvbdump 13 10m80 40 128 1`: CRC 1937985440, the 5m samples. Informational: the `copy`
   phase per fill in `profile` should be about 0.6k cycles (4 sps) and 0.3k (8 sps) below the
   matching 40 MS/s dump, the smaller overlap cache (model).
4. `dvbpace 13 10m 60`, `dvbpace 15 10m 60`, `dvbpace 13 5m 60`, `dvbpace 4 60`: `ESP_OK`;
   late fills, fatal and underruns 0; **`late_switches` 0 and `max_switch_late_cycles` 0**
   (were about 612 and 735-755); longest fill within 1.5k cycles of results/2026-10-16-next
   (56.9k, 56.9k, 43.0k, 41.4k).
5. `dvbpace 13 10m80 60`: `ESP_OK`, `dac_rate` 80000000, `step_words` 15740; late fills, fatal
   and late switches 0; longest fill at most 44,000 and smallest slack at least 3,000 cycles
   (model 42.6k and 4.6k). Underruns are expected 0 (ring model: 8,328 symbols of lead at 49%
   encoder load with two slots); if not, the two-slot ring is the cause, not the schedule.
6. `dvbpace 13 20m 5`: expected to fail cleanly with `ESP_ERR_TIMEOUT` and `fatal_late` 1
   (model: 56.5k against 47,220 + 1,152). Pass: that status, no reset, and `info` answers
   afterwards. This checks only that an overrun at 80 MS/s stops the run; it is optional.

## Keyed tests for the coordinator, inside the shielded box

Not run by this branch. Each needs the usual callsign, ID and attenuation rules.

- **K0, now, 40 MS/s, with the receiver** (cabled through at least 30 dB, as in
  results/2026-10-16-radio): `tx 2420 gap 15 0` (the start and end IDs take about 10.4 s of
  it), a 0.5 s capture at 61.44 MS/s between the IDs, then
  `capture.py <file> --format i16 --rate 61440000 --scale 2048 --signal gap`. It measures where
  the DAC really switches. Pass: every gap between 1 and 63 samples (inside the 64-sample lead);
  `off_grid` 0 apart from ID spaces. Report `lag_samples_median` (d) and the p1/p99 spread.
  d at or below 16 samples (0.4 µs) would allow lead 64 at 80 MS/s; a p99 under 32 samples and no
  long gaps would allow tail 256.
- **K1, after the ID is rate-aware, no receiver needed:** a keyed `10m80` run for 30 s (needs a
  `dvb` path at 80 MS/s, not on this branch). Pass: as `dvbpace 13 10m80`, plus `fe_gain_index`
  0, and longest fill within 2% of the radio-off run (the engine's 320 MB/s reads).
- **K2, after K1, with the receiver:** the `gap` probe at 80 MS/s, and a 0.5 s capture at the end
  of a 30 s keyed run (tone phase folded at the bank period, as in results/2026-10-16-radio), to
  check that the DAC and CPU clocks do not drift apart over a run. Both come from the same PLL
  in the clock tree as far as known, but this has never been measured for longer than 0.5 s.

## Next

The `gap` probe at 40 MS/s (K0). It is the only measurement that turns lead and tail from
inherited numbers into measured ones, and it runs on today's keyed path.
