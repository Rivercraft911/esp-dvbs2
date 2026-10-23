# TS input at 33.33 Msym/s: BBHEADER roll-off

**Result.** With TS input, QPSK at 33.33 Msym/s was shaped with roll-off 0.20, but its
BBHEADERs said 0.35 (MATYPE-1 0xf0). Both TS encoder paths hard-coded that byte. GCS input
already carried 0.20. Since bdf9cf6 the TS headers say 0.20 as well (MATYPE-1 0xf2), and the
board's dumps match the independent model with 0.20 headers, sample for sample.

| MODCOD | Firmware | Dump CRC-32 | Samples off, model at 0.35 | Samples off, model at 0.20 |
|---|---|---:|---:|---:|
| 11 (QPSK 9/10) | 43f5286, before | 2,939,266,175 | 0 | 89,782 |
| 4 (QPSK 1/2) | 43f5286, before | 649,464,914 | 0 | 359,787 |
| 11 (QPSK 9/10) | d5bcd6a, after | 1,374,526,537 | 89,782 | 0 |
| 4 (QPSK 1/2) | d5bcd6a, after | 2,840,379,987 | 359,787 | 0 |

Each dump has 1,278,029 samples: 16 frames at 12/5 samples per symbol, 80 MS/s.

## How it was checked

- Board: XIAO ESP32-S3 44:B1:76:B0:E5:54, radio off, peak 128, callsign N0CALL.
- Input: `spiinput quad ts replay`, which replays the eight presealed test messages.
- Dump: `dvbdump MODCOD 33m333 16 128 1`.
- The host CRC of the received bytes equals the board's CRC in every run.

The model is independent of the firmware:

- TS framing: `host/dvbs2.py` `ts_bbframes` with the given roll-off, applied to the replay
  stream `host/rx.py` `ts_messages(0, 8, True)`.
- Encoding: then `fecframe` and `plframe_phases`.
- Shaping: the convolution of `tests/reference/machine.py`.

This is the comparison `ts_replay()` in `tests/test_rational.py` makes against the host
harness, here applied to the board's files. `tests/data/ts_replay_crcs.json` pins the after
CRCs.

GCS replay at this rate was already 0.20 and did not change. After the fix the board gives
3,554,849,904 (MODCOD 11) and 186,984,531 (MODCOD 4), equal to the host harness.

The earlier keyed TS run at this rate (results/2026-10-23-tx-ts-input, 3679c73) carried 0.35
headers. LeanDVB still decoded 793 of 793 frames, so its decoder evidently does not act on the
RO field.

`runs.jsonl` holds:

- `before-model` and `after-model` rows: the comparisons above.
- `after` rows: the full board records of the four d5bcd6a dumps, TS and GCS.

## Correction to results/2026-10-23-tx-fill

That record says core 1's FreeRTOS tick "does no timekeeping (core 0 counts ticks). It only
checks time slicing."

The first half holds, but the tick does more than time slicing. It also performs the deferred
preemption core 0 leaves for core 1 (`xYieldPending[1]`) when a timeout readies a core-1 task.
With the tick off, such a task would not run until the next scheduling event or the end of
the run.

No firmware task is affected, because core 1 is reserved for the radio worker during a run:

- The worker runs at `configMAX_PRIORITIES - 2` and never blocks in a run.
- Every other TX task is pinned to core 0.
- ipc1, the one task above the worker, is woken by notification, which still preempts at once.

`tests/test_radio_duration.py` checks the task placement in the sources (64a3e99). It also
runs the production worker's tick disable and restore with injected failures (0de13ea).
