# 8PSK saved-record audit

All eight host LeanDVB loopbacks recovered **12 contiguous data frames, starting at frame 1**.
This corrects the original [run description](../2026-10-16-dvb-8psk/README.md), which
incorrectly says ordinary captures returned 14 frames. The raw records already reported 12;
no raw results or previous files were changed. The original files are retained as recorded.

The total is unchanged: 4,642,560 payload bits compared exactly, zero LDPC bit flips,
zero unsatisfied checks before decoding and zero BCH failures, with and without dummies.
All other evidence, source hashes and timing qualifications are in that original run.
The audit checks all eight counts, starting frame indices, matches and total compared bits;
its source SHA-256 is in this directory's `runs.jsonl`. No hardware was accessed.
