# QPSK startup headroom

The two-slot QPSK 26.67 Msym/s ring ran out during its first cold refill after the
opening station ID. A trace found three dummy frames there. Three slots
provide another frame of headroom through the existing allocation path, using
8,352 more internal bytes.

The `4209d1c-q3` image retains the original linker guards and qualified renderer
placement. Fixture: XIAO S3 → 40 dB attenuation → receive-only B200 at 2420 MHz;
TX gain 0, peak 48, RX gain 40 dB, profile off.

| Run | Payload | Exact frames | TX underruns |
|---|---|---:|---:|
| QPSK 9/10, 26.67 Msym/s, 1 | changing | 32 | 0 |
| QPSK 9/10, 26.67 Msym/s, 2 | changing | 31 | 0 |
| QPSK 9/10, 26.67 Msym/s, 3 | changing | 32 | 0 |
| 8PSK 2/3, 26.67 Msym/s reference | prepared, repeated | 46 | 0 |

Each run lasted 20 s including two 4.98 s KJ5SCH IDs. Minimum free internal
memory across the three QPSK runs was 41,527 bytes. Stock
LeanDVB recovered full headers and exact payload bytes with zero accepted parity
residuals or BCH corrections. Payload checks cover one 41.94304 ms capture per
run. The full transmission was not captured. The two earlier two-slot failures and
instrumented trace remain in `runs.jsonl`; the original wide-filter record is unchanged.
