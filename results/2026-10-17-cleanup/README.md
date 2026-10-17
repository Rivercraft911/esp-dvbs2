# Cleanup, radio off

exp/cleanup removes 6,321 lines (firmware 1,567, host 1,427, tests 973, docs 2,362) and 4,096 B
of static internal RAM, and passes 31 of 32 board checks in `host/check.sh`. `20m` now selects
the fastest measured kernel per modulation (6-symbol pulse for 8PSK, q4 kernel for QPSK); its
changed CRCs are recorded in docs/bank-fill.md. Every other kept waveform is unchanged. No RF.

Firmware `f5b7c3a`, ESP32-S3 at 240 MHz. `check.log` is the full run; the JSON records are next
to it. Every dump matched its CRC and decoded in LeanDVB; every pace was `ESP_OK` with zero late
fills and underruns; every `wifimem` probe stayed at or above the 22,351 B floor.

The one failure: 8PSK 9/10 at 5 Msym/s, encoder 220,154 cycles per frame mean and 226,763 live
max against 2% ceilings of 219,519 and 225,670 (2.3% and 2.5% above results/2026-10-17-qpsk-goal).
That mode's encoder has moved by similar amounts with code layout before (results/2026-10-17-next4);
it uses 21% of a 5 Msym/s frame, so no deadline is near.
