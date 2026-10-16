# Memory and bank-fill timing on the ESP32-S3

Firmware `adfaec6`, ESP-IDF v6.1, ESP32-S3 rev 0.2, 240 MHz, 8 MB octal PSRAM at 80 MHz.
`python3 host/board.py membench`, three runs, records in `runs.jsonl`. Every test repeats 7
times inside a run and reports the minimum. The three runs gave identical minimum cycle counts
for every test except `psram_write`, which varied by 0.1%.

| Test | Result | What it stands for |
|---|---:|---|
| `bank_store` | 2.25 cycles/sample, 154 µs per bank | writing a 16380-sample DAC bank, 32-bit stores |
| `bank_rows` | 2.63 cycles/sample, 179 µs per bank | copying 8-sample rows picked from a 32 KiB table |
| `bank_rows_or` | 5.88 cycles/sample, 401 µs per bank | same, OR of an I row and a Q row from two tables |
| `sram_memcpy` | 384 MB/s | internal RAM to internal RAM |
| `psram_read` | 59 MB/s | 1 MiB sequential 32-bit reads |
| `psram_write` | 35 MB/s | 1 MiB sequential 32-bit writes |
| `psram_to_sram` | 59 MB/s | memcpy of 1 MiB in 64 KiB pieces |

At 40 MS/s a new bank is due every 356.7 µs (docs/prior-art.md). Row copying fits on one core
with half the time to spare; the I|Q combine in plain C does not, so it is the first target for
the PIE vector instructions. The DAC reads 160 MB/s, nearly three times what PSRAM delivers, so
samples have to be produced in internal RAM.

The kernels run from IRAM with random row indices fixed by a seeded xorshift32. They time CPU
work only: the radio was off, so these numbers do not include bus contention from the DAC
engine reading the other bank. `psram_read` and `psram_to_sram` gave identical cycle counts,
which fits both being limited by the same cache-line refills.
