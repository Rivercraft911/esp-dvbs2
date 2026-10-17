# Integrated QPSK, offline checks

Firmware source: `a64e260`, after merge `7bcdb01` of next5 `298235f` and
qpsk-fill `81f0b4c`. The handoff commit adds the board script, its fake-board test
and this report. These are host, QEMU, build and derived allocation results.
**No integrated silicon timing or Wi-Fi measurement is recorded here.** No
serial port was opened, flash written or radio started.

`runs.jsonl` contains 169 records: 34 host-program passes, 65 stream configurations,
48 LeanDVB checks plus four detailed new-mode MER/decoder results, two QEMU
summaries, linked sections, build provenance and fourteen allocation models.
Host clocks and heap-size stubs are not board measurements. The retained log
hashes identify the local test outputs in the gitignored `notes/` directory.
Large IQ files remain there, not in the repository.

- All eighteen existing 40-frame CRC gates pass, unchanged. Every generated
  DAC word also agrees with the plain encoder and independently checked table.
- All eight QPSK MODCODs pass at both new rates. The four requested 16-frame CRCs
  are 3388792897 / 1271511239 (3 sps) and 3789273049 / 2081653259 (4 sps).
  Each new configuration decodes fourteen contiguous bit-exact frames with
  zero payload errors, LDPC checks/flips or BCH failures/corrections.
- New dump MER is 37.995 / 37.984 dB (MODCOD 4 / 11 at 3 sps) and
  41.219 / 41.206 dB (4 sps). These are host raw-sample measurements.
- Native stream tests exercise allocation failures, cleanup, borrowed workspace
  ownership, repeats and full physical-wrap cycles. Ring tests add forced dummy
  insertion and threaded producers at both bank geometries.
- QEMU: 140 FEC CRCs, fourteen PIE checks, 896 existing packed frames and 512
  quad frames with borrowed payload, all output alignments and guarded workspace
  tails. Shaper checks include 65,536 quad-copy slices, 19,368 QPSK slices and all
  prior shaper/glue cases. No QEMU cycle count is used as performance evidence.
- All 34 host programs pass. The board-script test runs its 34 fake commands and
  22 fake decodes, then injects command, CRC, decode and minimum-heap failures;
  the resulting four failures are retained and every later command still runs.
- IDF v6.1 builds with `IDF_COMPONENT_MANAGER=0`. The initially requested plain
  build failed in sandbox-denied `psutil`/`sysctl` discovery, before project
  compilation. The unused component manager is the only workaround.

The linked image recovers 3,328 B internal capacity against next5, and 768 B
against main `4f926aa`. Existing per-mode heap requests are unchanged. The
new 3-sps layout has no ring heap; the 4-sps layout borrows its workspace from
unused table storage. Both retain three ring slots, resident BCH and PIE buffer
alignment. Wi-Fi minima are predictions until measured.

The [handoff](../../docs/qpsk-goal.md) contains the per-mode allocation table,
cycle allowances, unresolved hardware gates and all commands/CRCs. The script
prints pass criteria before any board action:

```sh
bash host/qpsk-goal-board.sh
```

This is for the coordinator after a separate build/flash. It includes no keyed
command and requires the current firmware plus stored N0CALL. A successful
radio-off pace would still not qualify RF spectrum or receiver performance.
