# 8PSK transmitter, host verification

MODCOD 12..17 is wired into `dvb`, `dvbdump` and `dvbpace` at 5 and 10 Msym/s.
QPSK syntax is preserved. The shaper accepts raw encoder phases, and underruns insert
3,330-symbol dummy PLFRAMEs. Roll-off is 0.35 in both pulse and BBHEADER.

**No hardware was accessed.** These are host receiver tests, QEMU instruction checks and
planning models. Source SHA-256 hashes, built image hashes and receiver identity are in
`runs.jsonl`. Base revision is `058387aca9f4fc6d9c487b08f9355e959dfd9f1b` on `exp/dvb-8psk`, with
uncommitted changes. The sandbox denied Git index.lock creation, so a new source commit
could not be made in this session. The hashes identify the tested files exactly.

| Evidence | Result |
|---|---|
| Host test suite | all 19 `tests/test_*.py` scripts pass; both new tests are in CI |
| Encoder regression | 140 C phase CRCs unchanged (`tests/test_fecbench.py`) |
| Parser | 133 invalid requests rejected before dispatch; dump/pace call only their own entry point |
| QEMU raw sample glue | 1,520 cases at 4/8/40 sps; zero word mismatches, guards intact |
| Existing QEMU oracle | 30 PIE/C and independent Python table/output checks pass |
| Dummy encoder | all 3,330 C phases equal independent Python formulas; guards intact |
| Ring host model | mixed data/dummies, starvation and two threads at 4/8/40 sps pass |
| ESP-IDF v6.1 | firmware and shaper QEMU harness build cleanly |

Eight host loopbacks cover MODCOD 13/15, 4/8 sps, with and without dummies before data
frames 8, 8, 9. `tests/test_tx_8psk.py` generates 16 distinct data frames using the independent
Python encoder, models the transmitter's scaled integer table (peak 128), and invokes
`host/onair.py --stream 16` with the separate LeanDVB executable. Every recovered payload
matches: 4,642,560 bits compared, zero LDPC bit flips, zero
unsatisfied checks before decoding and zero BCH failures. Each ordinary capture returns
14 payload frames; each dummy capture returns 12. Capture endpoint loss is allowed.

Synthetic 30 dB Es/N0, +73 ppm, carrier +0.011 Rs: recovered MER 29.958/29.956 dB at
5/10 Msym/s. Scaled table models with consecutive dummies give 42.922/42.234 dB MER.
These numbers exclude RF, analog filters, DAC hold and active memory-bus contention.

The bank period is `(16380 - 64 - 2048)/40 MS/s = 356.7 us`, or **85,608 cycles** at
240 MHz (`radio_s3.h:7-11`, measured clock in `2026-10-16-dryrun/runs.jsonl:1`).
The raw n8 kernel projection starts from measured 1.776 cycles/sample; raw n4 starts
from modelled 2.295, adding one instruction per symbol. Including legacy fill overhead,
extra symbol copying and an assumed 2k-cycle glue reserve gives:

| Planning estimate | 5 Msym/s | 10 Msym/s |
|---|---:|---:|
| Fill, cycles/bank | 54.2k (63% of deadline) | 73.1k (85%) |
| Fill slack, cycles | 31.4k | 12.5k |
| 2/3 encoder, kernel-only / cold-buffer scenario load | 20% / 41% | 40% / 83% |
| 5/6 encoder, kernel-only / cold-buffer scenario load | 29% / 52% | 59% / 105% |

The scenarios are not measurements or bounds. They add 10 instructions per payload byte
plus full measured PSRAM transfer costs to the measured encoder maximum, so some existing
buffer cost is counted twice. **10 Msym/s 5/6 has an open encoder-throughput risk.** Fill
contention, Wi-Fi memory needs and interrupts also remain unverified. The earlier 340k-cycle
stream excess included payload generation and PSRAM output as well as phase conversion;
it was not an isolated measurement of the removed pass.

Exact [board commands and pass criteria](../../docs/hardware.md#8psk-radio-off-checks)
cover both MODCODs and rates, 60 s pace with zero late fills and underruns, full-rate MER,
QPSK regression and forced underruns. Long derivations and model scripts are retained in
ignored `notes/dvb-8psk.md` and `notes/budget.py` in this worktree.

Reproduce without hardware:

```sh
python3 tests/test_tx_8psk.py --loopback --receiver /path/to/leandvb --out /tmp/new-loopback.jsonl
python3 tests/test_tx_samples.py /path/to/qemu-uart.log
```

The QEMU build/run instructions are in `tests/shaper_pie/README.md`. Saved result records
are immutable. Board and RF evidence must go in a new run directory.
