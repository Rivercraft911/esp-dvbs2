# Encoder candidate, offline

**8PSK 9/10 models at 101,466 cycles/frame including a 10k-cycle input copy;
QPSK 9/10 at 99,787. Neither is a silicon timing result.** Source `ef481ba`,
baseline `da97362`, ESP-IDF v6.1 / Xtensa GCC 15.2.0. No hardware was accessed.

QEMU instruction traces, not its CCOUNT, supply executed instructions to an
LX7/PIE interlock model (TRM v1.8, 1.7, Table 1.7-2). Cache stalls, SRAM
contention, interrupts, station ID and stream bookkeeping are excluded. The
old QPSK trace totals 163,672 cycles without payload versus 168,929 from the
measured stream, so this replay is not an exact timing prediction.

At 240 MHz and `Rs = 100e6/3 symbols/s`, `cycles/frame = 240e6*N/Rs`:
159,796.8 for 22,194-symbol 8PSK; 239,630.4 for 33,282-symbol QPSK (EN 302
307-1, 5.5/Table 11). Integer 85% ceilings are 135,827 / 203,685 cycles.

| Mode | Model + 10k input, cycles/frame | Kernel inflated 25% + 10k | 85% ceiling |
|---|---:|---:|---:|
| 8PSK 9/10 | 101,466 | 124,333 | 135,827 |
| QPSK 9/10 quads | 99,787 | 122,234 | 203,685 |
| 8PSK 2/3 | 120,622 | 148,278 | 135,827 |
| 8PSK 5/6 | 125,676 | 154,595 | 135,827 |

The sensitivity column is not a bound. 2/3 and 5/6 do not pass it.
BCH falls from 70,335 to 43,324 model cycles for t=8, and QPSK mapping from
65,738 to 18,862. No LDPC algorithm or output format changes.

Heap requests are unchanged. Linked internal heap starts 784 B later;
extrapolating the previous tight Wi-Fi minimum gives **21,567 B**, against a
20,480 B gate. Only a board probe can confirm that. The 8PSK 9/10 flash-table
union is 31,488 B; five modelled cache sets exceed eight ways. This is a
placement warning, not a measured miss count. Wider-pulse RAM is additional.

All 57 previous waveform CRCs survive. A new 16-frame 8PSK 9/10 dump has CRC
3,782,178,445; LeanDVB recovers 13 contiguous bit-exact frames with zero
corrections/errors. QPSK 26.67 Msym/s recovers 14 of 16. QEMU passes 140 phase
CRCs, 14 stage checks, 896 packed frames and 512 quad frames. Firmware builds.

After the coordinator builds/flashes the committed branch:

```sh
host/check.sh --encoder --plan
PYTHON=/Library/Frameworks/Python.framework/Versions/3.13/bin/python3 \
  RX=/path/to/leandvb host/check.sh --encoder notes/moon-encoder-board
```

[The gates and derivation](../../docs/encoder.md#encoder-candidate-for-3333-msyms)
cover six decoded dumps, four 30 s paces, one PSRAM regression and three Wi-Fi
probes, continuing after failures. Mean and live maximum use a complete
post-generation interval plus the estimated input copy. Real SPI timing,
33.33 Msym/s shaping and roll-off 0.20 framing remain integration work.
Next: measure those intervals with both cores active, then the actual SPI
stream; adjust flash-table placement if the cache counters justify it.
