# Clean third SPI run

Firmware **39d9c04**, ESP-IDF v6.1, 240 MHz, 8 MiB PSRAM, N0CALL. The coordinator
power-cycled the board and ran `WIFI_ALL=1 SECONDS_PER_RUN=30 bash host/spi-in-board.sh`.
Radio off. No other job reset the board. Earlier interrupted runs are excluded.
These are selected, unaltered records from `notes/spi-in-board-20261017-042750/runs.jsonl`;
the supplied console log is `notes/board-round3-clean.log`. No new board run was made.

| Demo, 8PSK 2/3, 10 Msym/s | Mean cycles/frame | RX cycles/frame | Copy cycles/frame | Underruns |
|---|---:|---:|---:|---:|
| TS loopback, 30 s | 454326 | 85365 | 21461 | 5839 |
| TS replay, 30 s | 250454 | 30640 | 4296 | 0 |
| GCS replay, 30 s | 181925 | 30380 | 4293 | 0 |

RX and copy windows include preemption; copy is already part of RX. The loopback
sender ran on core 0 at priority 6, above the priority-5 encoder. Its scheduled
CPU fraction was `13001058 / 40481666 = 32.1%`. This does not directly attribute
cycles to individual encoder stages. The encoder's BCH mean was 182824 cycles
with loopback and 84961 with TS replay; payload, BB and map also grew. Buffers,
payload, context and workspace were internal RAM in both runs. Copy granularity
already spans whole remaining message/frame fragments, rather than single bytes.

The QPSK 9/10 paces used replay, with no SPI3 sender. Their TS/GCS means were
244802/208727 cycles/frame; synthetic input was 222001. Thus this run does not
establish a modulation-specific loopback slowdown. All selected input paces used
nibble BCH; synthetic 8PSK uses resident byte BCH.

The demo frame budget is `240e6 cycles/s * 22194 symbols / 10e6 symbols/s =
532656 cycles/frame`. The fault-6, 2 s starvation run also exceeded that budget:
live maximum 593506 cycles/frame. Its intentional underruns are expected.

The full console summary passed dump/decode, replay, throughput 22/46.6 Mb/s,
capacity, faults 1–5, input-off and synthetic regressions. Gates failed for the
normal demo pace and starvation live maximum. `gcs-wifi-5 26m667` timed out after
90 s, with no returned record. The log contains no below-gate heap number for
that command. Its TS counterpart reported 34635 B for both Wi-Fi-up free heap
and minimum, above the 20000 B gate. The timeout's cause remains unknown.
