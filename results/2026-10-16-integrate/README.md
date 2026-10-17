# shaper-next and station-id on top of 8PSK, radio off

**Open: the test callsign `N0CALL` is still stored in the board's NVS.** The board stopped
answering on USB after it was moved at 18:11 local (01:11 UTC), during a 600 s 8PSK paced run,
so `callsign clear` could not be sent. Run `callsign clear`, then `callsign` and `info`, before
anything else; until then `tx` and `dvb` would not refuse.

Both branches are merged and every radio-off check that ran passes on the board. The transmitter
now uses the hold-corrected taps. That lowers the 10 Msym/s **digital dump** MER from 42.21 to
40.67 dB, as the model predicts, while the same samples with an ideal DAC hold applied go from
37.62 to 41.63 dB. The 5 Msym/s schedule still holds with the encoder running (0 late fills,
6,020 cycles smallest slack), and with the CW ID wrapper (5,163). Two problems showed up only on
the board and are fixed: shaper-next's C kernel made the transmitter's tail path 25% slower, and
`iddump` tripped the task watchdog. No RF was transmitted.

## What ran

ESP32-S3 at 240 MHz on `/dev/cu.usbmodem101`, radio off, no SDR. Every completed command is in
`runs.jsonl`; LeanDVB decodes, Morse decodes and MER models are in `decodes.jsonl`. Dumps and
logs are in ignored `notes/captures/2026-10-16-integrate/`.

| Firmware | Commit on exp/integrate | Used for |
|---|---|---|
| `1de0807` | merge shaper-next | first membench, dumps and pace (kept: shows the C slowdown) |
| `b81f756` | fill_c: four words again | all merge-1 checks below |
| `19b530f` | merge station-id | first `iddump`, which failed |
| `e4405ff` | iddump: let IDLE1 run | all merge-2 checks below |
| `72a7016` | main, before both merges | same-session baseline, built from `git archive` with `version.txt` set to the hash |

Host side: all `tests/test_*.py` scripts pass after each merge (21, then 23), `idf.py build` has
no warnings, and the QEMU harness (`tests/shaper_pie`) passes 324 PIE kernel cases and 1,520
raw-phase sample-glue cases on `b81f756`. The shaper-next cycle model reproduces its numbers
exactly on the merged assembly once it skips `.align` padding.

## Which taps the transmitter uses

The transmitter uses the **hold-corrected** a35 tables. The radio sends the DAC output, and a
DAC holds each sample for 1/Fs, which multiplies the spectrum by sinc(f/Fs). At the band edge
f = 1.35 Rs/2 that is -0.10 dB at 5 Msym/s (Fs/Rs = 8) and -0.41 dB at 10 Msym/s (4).
EN 302 307-1 V1.4.1 Annex A (p. 37) requires an x/sin x correction when the shaping filter is
fed rectangular instead of impulse inputs; the DAC hold is the same rectangular pulse at the
sample period. The correction costs no cycles. A `dvbdump` has no hold, so it sees the
pre-emphasis without the droop that cancels it.

MER in dB, MODCOD 13, peak 128, measured with `onair.py --mer` (RRC 0.35 receiver):

| Rate | Taps | Model, no hold | Board dump | Model, ideal hold | Board dump + ideal hold |
|---|---|---:|---:|---:|---:|
| 5 Msym/s | uncorrected (3119ce4) | 43.15 | 43.15 | 42.03 | 42.03 |
| 5 Msym/s | corrected (b81f756) | 43.79 | **43.79** | 43.30 | 43.29 |
| 10 Msym/s | uncorrected (3119ce4) | 42.20 | 42.21 | 37.61 | 37.62 |
| 10 Msym/s | corrected (b81f756) | 40.65 | **40.67** | 41.62 | 41.63 |

Model: `tests/test_tx_8psk.py`'s waveform with the truncated peak-128 taps, 3 frames. Hold:
the dump's spectrum times sinc(f/Fs) at Fs = 40 MS/s; its half-sample delay is left to timing
recovery. Board numbers agree with the model within 0.02 dB, so the lower 10 Msym/s dump MER
is the expected effect of the correction, not a fault. At 5 Msym/s the correction is smaller
than the nine-symbol truncation error and happens to lower the symbol-spaced ISI even without
a hold (pulse-only MER 46.45 to 47.56 dB), so the dump MER rises slightly. Both corrected dumps
stay above the 38 dB dump floor in `docs/hardware.md`, which now states the new expectations.
The uncorrected 10 Msym/s signal would have been at 37.6 dB after a real hold.

The hold model is ideal. The S3's DAC type and its analog filters are undocumented, so only a
cabled RF capture can show whether the correction helps the actual output.

## Merge 1: shaper-next

| Check (b81f756) | Result | Pass |
|---|---|---|
| `membench` + `host/shaper.py verify` | all 30 table and output checks match, including n4 `_all`, `_rows`, `_rows_all` | yes |
| PIE timing vs shaper-next model | n4 37,592 (model 37,576), staged n4 25,290 (25,275), row staging 32,828 (32,814), n5 68,589 (68,575), n8 29,088 (29,065) | within 0.1% |
| `dvbdump 4 40` | CRC match, 0 overlap mismatches, 38 frames bit-exact from frame 0, MER 43.81 dB at 4 MS/s | yes |
| `dvbdump 13 5m 40 128 1` | CRC match, 36 frames bit-exact from frame 1, 0 LDPC flips, MER 43.79 dB | yes |
| `dvbdump 13 10m 40 128 1` | CRC match, 36 frames bit-exact from frame 1, 0 LDPC flips, MER 40.67 dB | yes |
| `dvbpace 13 5m 60` | 168,209 fills, 0 late, 0 fatal, 0 underruns, longest fill 79,330, smallest slack 6,020 cycles | yes |

`membench`'s memory section ("internal allocation failed") did not run. It needs two 32 KiB
internal blocks, and the largest free block has been 31,744 B since at least `35739b5`, the
same on `72a7016`. The shaper section, which `verify` reads, runs after it.

Fill timing at 5 Msym/s, whole bank, deadline 85,608 cycles:

| Firmware | Longest fill | Smallest slack |
|---|---:|---:|
| 3119ce4, results/2026-10-16-8psk-board | 79,627 | 5,713 |
| 72a7016, this session (firmware identical to 3119ce4) | 80,052 | 5,300 |
| 1de0807, merge with per-word C kernel | 80,216 | 5,136 |
| b81f756, merge 1 final | 79,330 | 6,020 |
| e4405ff, with N0CALL and CW ID wrapper | 80,190 | 5,163 |

Identical firmware varied by about 400 cycles between runs, so these differences are within
run-to-run spread. The margin is still thin: 6% of the deadline.

**C kernel.** shaper-next wrote one word per iteration so `shaper_fill_c` could do 5
samples/symbol. That kernel also writes the transmitter's 0..5-symbol tail after each PIE
call. On the board a bank-sized C call at 8 sps went from 245,155 cycles (72a7016) to 306,543
(1de0807). `b81f756` keeps the old four-word loop for multiples of four: 259,502 cycles at
8 sps, 357,681 at 4 (328,996 on main), 176,018 at 40, and 365,233 for n5. The output words are
unchanged (same checks, same dump CRCs). The remaining 6% to 9% gap to main is code
generation. The tail is at most 40 words per call and two calls per bank, so the estimated
cost is about 70 cycles per bank (0.88 cycles/word more than main).

## Merge 2: station-id

| Check (e4405ff) | Result | Pass |
|---|---|---|
| `info`, `callsign` before setup | `callsign:""` | yes |
| `iddump N0CALL` (nothing stored) | 208,800 B, CRC 1965033598 equal to the host and to the portable C generator compiled on the host, 208,800,000 DAC samples, 0 overlap mismatches, longest CW fill 36,458 cycles | yes |
| `host/morse.py --rate 10000 --tone 0 --expect N0CALL` | `N0CALL`, 21 marks, match | yes |
| `callsign n0call`, `callsign --reset`, `info` | `N0CALL` stored, survives reset, shown by `info` | yes |
| `iddump` (stored callsign) | same CRC, decodes `N0CALL` | yes |
| `dvbdump 4 40` + `onair.py --callsign N0CALL` | CRC match, 38 frames bit-exact including every 40-byte header, MER 43.82 dB; without `--callsign` the payload does not match | yes |
| `dvbdump 13 5m 40 128 1` + `--callsign N0CALL` | 37 frames bit-exact including headers, 0 LDPC flips, MER 43.78 dB | yes |
| `dvbpace 4 60` | 197,477 fills, 0 late, 0 fatal, 0 underruns, slack 44,281, `id_count` 2, 2,817,600,000 samples (70.44 s) | yes |
| `dvbpace 13 5m 60` | 197,477 fills, 0 late, 0 fatal, 0 underruns, longest fill 80,190, slack 5,163, `id_count` 2 | yes |
| `dvbpace 4 600` | 1,725,989 fills, 0 late, 0 fatal, 0 underruns, `id_count` 3 (nine-minute repeat), 24,626,400,000 samples (615.66 s), longest fill 68,762, slack 16,595 | yes |
| `dvbpace 13 5m 600` | interrupted by the move; no record | not run |
| `callsign clear`, `callsign`, `info` | board not reachable | **not done** |

Refusal to key without a callsign was tested only on the host (`tests/test_tx_command.py`),
now also for `dvb 2400 13 5m 1` and `dvb 2400 15 10m 1`. No `tx`, `dvb` or `chain` command was
sent to the board.

The longest CW fill, 36,458 cycles, is 11% above the 32,917 the station-id model gave for the
largest of its eight QEMU cases; that model was not exhaustive. It is 43% of the bank deadline.
Station-id code and NVS cost 9,881 B of free internal heap at boot. The minimum free internal
RAM during `dvbpace 13 5m` fell from 53,735 B to 41,994 B.

The 600 s QPSK run's longest whole-bank fill, 68,762 cycles, is 27,700 above the 60 s run's
41,061, while its longest payload fill call stayed at 38,786. CW and payload never fill a full
bank each, and in `station_id.c` every CW/payload boundary sits in seven dots of silence, so the
extra time is not explained by the fill arithmetic. It may be the periodic ID boundary at 540 s
or a rare event that a 60 s run does not catch. At 5 Msym/s, with 5 to 6k cycles of slack, the
same excursion would be a late fill. The 600 s 8PSK run that would have shown this was
interrupted.

**iddump watchdog.** On `19b530f` the first `iddump` lost its final record and failed Morse
decoding. Its sink keeps one sample in 4000, so the dry run never blocked on USB and the idle
task on core 1 starved for more than the 5 s task watchdog. The watchdog's log was written into
the binary stream at byte 135,168. `e4405ff` yields one tick every 64 banks in the dry run.
That bug was on exp/station-id itself, which had never run on a board.

## Board lost

The coordinator moved the board into a shielded box behind a USB bulkhead at 18:11 local, within
a minute after `dvbpace 13 5m 600` started at 18:10:46. `board.py` timed out after 720 s without
a record. Since then the device enumerates (303A:1001, serial CC:BA:97:09:F7:E8) but returns no
serial data: `info`, `callsign`, a raw read and esptool (default, `usb-reset` and `no-reset`
connects) all got nothing, and OpenOCD failed reading a USB string descriptor. That points at
the USB link, not the firmware, but it is not proven. Per the coordinator, the 600 s 8PSK run
was not repeated.

## Not shown

- No RF. The DAC engine's reads of bank memory, the analog response and the real ID spectrum
  are untested; the hold correction is justified by an ideal model only.
- 10 Msym/s still misses its fill deadline (results/2026-10-16-8psk-board); not rerun here.
- The 80 MS/s prototypes from shaper-next are verified for correctness and raw kernel timing
  only. No producer, queue or 80 MS/s schedule exists.
- `membench`'s memory section does not run with the current heap layout (above).
