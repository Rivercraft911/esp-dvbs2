# Receive demo, files only

**Clean 10 Msym/s video works. The installed hard LDPC decoder does not meet the
requested noise envelope.** The complete clean backend ran for 60 s at 10 Msym/s,
with 27,111 good FEC frames, zero failed frames and 801 decoded video pictures.
An unpaced run of the same backend reached **2.28× real time**. No SDR, serial
port or other lab device was accessed. Browser rendering is not included.

Use [the runbook](../../docs/demo-rx.md) for setup, one-command replay, live E200
commands, telemetry and fallback receivers. These results qualify file workloads,
not RF reception or the proposed CM5 panorama encoder.

## Decoder throughput

Real-time factor (RTF) is `input complex samples / Fs / wall seconds`, so larger
than 1 means faster than the input clock. Values below are medians of three
runs, each with at least 3 s of signal. The complete commands, elapsed and CPU
times, hashes, status counts and rates are in the named JSONL files.

8PSK 2/3, normal frames, pilots, roll-off 0.35, RRC sampler, `--rrc-rej 20`,
`--fastdrift`, `--ldpc-bf 200`, diagnostics enabled:

| Input | Clean | Es/N₀ 18 dB | Es/N₀ 15 dB | Es/N₀ 9.62 dB |
|---|---:|---:|---:|---|
| 5 Msym/s, original 40 MS/s dump | 2.75× | 2.84× | 1.01×, 3/2028 failed | no payload |
| 5 Msym/s, FIR reduced to 10 MS/s | 4.11× | 3.82× | 1.11× | no payload |
| 10 Msym/s, original 40 MS/s dump | 2.08× | 1.69× | 0.52× | no payload |
| 10 Msym/s, FIR reduced to 20 MS/s | 2.26× | 2.01× | 0.58× | no payload |

All attempts succeeded in other numeric cells. At 12 and 9.62 dB every attempt
failed for both rates and both input rates. Failure rows still consume input
quickly; their RTF is **not** usable decode throughput. Each 10 Msym/s 2-sample
noise point has 4068 attempts over three identical timing repeats. Raw files:
`8psk-{5,10}m-{native,2sps}.jsonl`.

QPSK 3/4, synthesized TS fixtures, two samples per symbol, otherwise same flags:

| Symbol rate | Clean | Es/N₀ 15 dB | Es/N₀ 12 dB | Es/N₀ 10 dB | Es/N₀ 7.03 dB |
|---|---:|---:|---:|---:|---|
| 5 Msym/s | 5.10× | 4.73× | 4.43× | 2.05× | no payload |
| 10 Msym/s | 2.69× | 2.47× | 2.25×, 3/3363 failed | 1.05×, 3/3363 failed | no payload |

All attempts succeeded in other numeric cells. Raw:
`qpsk-{5,10}m-2sps.jsonl`. QPSK has more useful margin at the measured noise
levels. Its 10 dB point leaves little whole-pipeline margin at 10 Msym/s.

## Noise definition and scope

`Pn = mean(|x|²) × (Fs/Rs) × 10^(-EsN0_dB/10)` is complex sample noise power.
Each independent Gaussian I and Q component has variance `Pn/2`.
Thus `Es/N₀ = sample SNR + 10 log10(Fs/Rs)` for the ideal matched filter. Noise
is seeded with 20261017 and generated before timing. Clean means no added
noise. Higher Es/N₀ tends to the clean limiting case. The unit test measures the
noise variance and checks the sample-rate factor.

[EN 302 307-1 V1.4.1 clause 6, Table 13, printed p. 36](https://www.etsi.org/deliver/etsi_en/302300_302399/30230701/01.04.01_60/en_30230701v010401p.pdf)
gives ideal thresholds of 6.62 dB for 8PSK 2/3 and 4.03 dB for QPSK 3/4.
The 9.62/7.03 dB tests are 3 dB above those ideal thresholds, not above an
established threshold of this hard decoder. The standard assumes soft iterative
LDPC decoding and perfect synchronization. Neither mode succeeds there in this
build. The finite repeated recordings do not establish a quasi-error-free
receiver threshold or an RF link budget.

Each original 8PSK dump contains only 40 frames. The same waveform and noise
period are repeated, so timing repeats are not independent error-rate trials.
FEC success means BCH did not report failure. It is not a measured bit-error
rate. An additional independent check (`payload-check.jsonl`) recovered a whole
214,800-byte, 40-frame known payload bit for bit, both clean and at 15 dB,
after acquisition, using three repeated dumps and `--fd-bbf`.

## What costs time

* **LDPC correction:** at 10 Msym/s and original 40 MS/s, 15 dB changes RTF from
  2.08× clean to 0.52× while recovering all attempted frames. Disabling LDPC
  correction reaches 1.49× but loses every frame. That is a cost comparison,
  not a working fast mode (`profile-ldpc0.jsonl`).
* **Front end:** clean 10 Msym/s at 80, 40 and 20 MS/s reaches 1.60×, 2.08× and
  2.26× respectively. At 5 Msym/s, 40 to 10 MS/s improves 2.75× to 4.11×.
  This includes sample conversion, scheduling, memory traffic and different
  matched-filter work. It does not isolate disk I/O. System CPU time is only
  about 3–9% of wall time in these clean tests. Total child CPU time is close
  to one CPU's wall time; the 14 cores are not automatically used by this path.
* **Resampling:** enabling LeanDVB's default `--resample` at 40 MS/s lowers
  clean RTF to 1.51× and its estimated MER from 26.8 to 19.6 dB. At 15 dB all
  frames fail. These filter settings cannot be recommended (`profile-resample`).
  Offline SciPy FIR decimation in the 2-sample cases is outside decoder timing;
  its cost is recorded separately. A live E200 should deliver the requested
  10 or 20 MS/s directly, rather than rely on that offline preparation.
* **Other switches:** linear sampling reaches 1.94× clean and lower MER, so it
  does not improve the complete decoder. Signed-16 input at 20 MS/s reaches
  2.35× clean and 0.58× at 15 dB, with all attempted frames recovered. It avoids
  float IQ pipe expansion but does not solve weak-signal cost.

`profile-s16.jsonl` is retained as a **superseded invalid comparison**. The
noisy case reinterpreted complex128 as float32 before integer conversion.
`81880fb` fixes it and adds a regression test; use `profile-s16-fixed.jsonl`.
No raw measurement files were rewritten.

## Complete backend

`paced60.jsonl` records a 60.175 s signed-int16 playback at 20 MS/s and
10 Msym/s 8PSK 2/3. It received 145,394,688 TS bytes, ended at 19.27 Mb/s,
kept all 59 probes after startup healthy, and had at most 89 ms cumulative
input deficit at those one-second probes. There were no failed FEC frames.
`unpaced5.jsonl` records the 2.28× float-IQ backend run with zero failed frames.
This includes file input, LeanDVB, TS recording, spectrum snapshots and FFmpeg
JPEG output, but excludes browser rendering and Ethernet SDR I/O.

The synthetic H.264 workload is 960 × 480, 15 frame/s, target 800 kb/s, inside
a constant-rate 19.3566 Mb/s TS mux. It is not a 4K panorama decode benchmark.
Repeated video timestamps and TS continuity counters produce join warnings and
some lost video frames, even though circular pulse shaping keeps FEC continuous.
The paced run's 801 pictures are 13.3 pictures/s, not proof of uninterrupted
15 frame/s camera output. Fixture hashes and FFmpeg commands are in each record.

Derived capacity is `R_TS = (Kbch - 80) / L_PL × Rs`, in bit/s. Existing
`dvbs2.py` tables and PL-frame formulas give 42960 data bits / 22194 symbols
for 8PSK 2/3, or 48328 / 33282 for QPSK 3/4 with pilots. At 10 Msym/s these
are 19.3566 and 14.5208 Mb/s. Occupied bandwidth is `(1 + 0.35) × Rs`, hence
13.5 MHz. Inputs follow standard clauses 5.1.5, 5.3 and 5.5, and Annex A.
The noise-free and two-sample limits are covered by existing IQ tests.

## Reproduce and hand off

Host: user-reported Apple M4 Pro; Python reports 14 CPUs, arm64 macOS 15.7.9.
NumPy 2.2.3 and SciPy 1.15.2. This was an ordinary desktop session, not CPU-
pinned or thermally controlled. Three repeats expose some timing variability;
all per-trial numbers remain available. One receiver benchmark ran at a time.
Temporary-file creation and noise generation are excluded from decoder timing.

Receiver: external patched LeanSDR work revision
`84c59e1c7a1a79338d5722d63f28640cc9d350f3`, with binary SHA-256 in each decoder
record. No external soft LDPC helper was used or qualified. The conclusion is
about this installed hard-decoder path, not a fundamental limit of the Mac.
Host code revisions `519dfd7` and `81880fb` are recorded. Dirty flags include
uncommitted results/dashboard documentation during the run.

Original 40 MS/s dump firmware is `3119ce4`, documented in
[the original board run](../2026-10-16-8psk-board/README.md). The 80 MS/s dump is
from `next5/notes/next5-board-20261017-011526/13-10m80.iq`, whose `runs.jsonl`
records firmware `298235f`. Full paths and capture hashes are in the raw records.
The QPSK and full-video fixtures use the host reference, with no board firmware.
`runs.jsonl` indexes every raw record file with its hash and validation status.

```sh
export LEANDVB=/Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb
python3 host/rxbench.py /Users/river/Documents/stanford/SSI/esp-dvbs2/notes/captures/2026-10-16-8psk-board/13-10m.iq \
  --receiver "$LEANDVB" --firmware 3119ce4 --sample-rate 40000000 \
  --symbol-rate 10000000 --decimate 2 --seconds 3 --repeat 3 \
  --out notes/repeat-rx.jsonl
python3 tests/test_demo_rx.py
RX_INTEGRATION=1 python3 tests/test_demo_rx.py
```

All 13 receiver tests passed, including video PID payload comparisons, JPEG/JSON
endpoints, paced IQ across loops, stale lock, full-scale spectrum, noise units,
telemetry, signed-IQ conversion and burst/silence rate rejection. Existing
`tests/test_decode.py`, `tests/test_iq.py` and `tests/test_sdr.py` passed.
JavaScript syntax passed `node --check`.

Remaining: coordinator E200 rate/continuity qualification, attenuated RF receive,
SPI TS framing and the actual panorama codec/resolution. Browser-origin approval
was declined, so the rendered view, drag, zoom and full-screen controls still
need visual verification. Hardware and software alternatives with their own
published limits are linked in the runbook. No RF or browser performance is
claimed from these file tests.
