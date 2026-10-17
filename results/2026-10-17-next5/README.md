# QPSK encoder integrated with next4, radio off

Merge `a11b7b5` has parents main `4f926aa` and QPSK encoder `8437931`.
All eighteen requested dump CRCs pass. All 31 host test programs pass, including
the board-script test. No serial port was opened, firmware flashed or RF transmitted.

[runs.jsonl](runs.jsonl) contains 72 records with source revisions, log hashes,
individual host outcomes, all eighteen dump checks, QEMU summaries, linked RAM,
clearly labelled supplied board measurements and the Wi-Fi heap estimate.
The integration was checked before its handoff documentation commit; firmware
source is the merge's `firmware` tree recorded in the first row.

The real host stream harness compares every DAC word with the portable encoder,
checks repeated commands and all allocation failures, and completes the physical
wrap schedule at both 40 and 80 MS/s. Optional LeanDVB checks also pass all
32 fast-QPSK configurations (MODCOD 4–11, both samples/symbol, both DAC rates):
38 contiguous bit-exact frames each, no LDPC/BCH corrections or payload errors.
These are host-generated samples, not captures from the integrated board.

ESP32-S3 QEMU passes all four harnesses: encoder (140 FEC CRCs, 14 PIE checks,
896 guarded packed frames and workspace-tail checks); shaper (324 kernels,
3,360 sample glue, 39,170 pack/unpack, 864 copies, 24 block-pulse and 162
short-pulse cases); radio (2,031,488 samples across both geometries); APSK
(1,760 checks, zero failures). QEMU cycle counts are not silicon timing.

The requested plain IDF v6.1 build was attempted and blocked by the sandbox's
denial of the unused component manager's `psutil`/`sysctl` process lookup.
The firmware and every harness build successfully with `IDF_COMPONENT_MANAGER=0`.
No SDK or project configuration was changed.

Linked internal RAM grows 2,560 B relative to main, with unchanged DRAM data/BSS.
All per-mode allocations preserve their mapper, workspace and ring policy.
QPSK gains 2,576 B of linked capacity relative to its branch, and the QPSK fast
windows share the internal workspace tail. See [RAM derivations](../../docs/next5.md).
The tightest old Wi-Fi mode, `17 5m`, is estimated at 19,019 B versus its
20,000 B script gate. That is a predicted failure, pending a board measurement.

The script is [host/next5-board.sh](../../host/next5-board.sh), about 12 minutes.
It lists criteria before issuing commands, saves every IQ dump, checks all three
CRCs and file sizes, runs the exact six paces and three Wi-Fi probes, and reports
all 29 outcomes. Its fake-board test verifies successful operation and continued
execution after a command error, a corrupt saved CRC and insufficient heap.
The real board script has not been run during this integration.
