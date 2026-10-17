# SPI input, second board run

Firmware `609856f`, ESP-IDF v6.1, ESP32-S3 at 240 MHz, radio off. The coordinator
ran `WIFI_ALL=1 SECONDS_PER_RUN=30 bash host/spi-in-board.sh`. `runs.jsonl` is the
unchanged coordinator file, including command strings and host timestamps.
The console summary reports 196 PASS steps and 35 failed record gates.

The 30 s TS loopback demo inserted 23,399 dummies despite zero transport errors.
Encoder active mean/live maximum were 375,739/433,311 cycles, below its 532,656-cycle
frame budget. READY blocked the sender 9,994 times for 10,009 encoded frames.
An untimed demo dump passed exact payload and independent decoding.

TS/GCS replay at 80/3 Msym/s passed with zero underruns. The 257,061-cycle mean
that failed the 254,607-cycle reserve target was **synthetic**, after `spiinput off`.
Verified standalone loop rates were 22.020, 46.768 and 57.650 Mb/s at offered ceilings
22, 46.6 and 87 Mb/s. The last did not reach its ceiling. This measures one-board
pattern construction, transmit DMA, receive and verification together.

Wi-Fi minima fell to 17,099 B at QPSK `20mq4` and 18,611 B at nine-symbol 8PSK
`10m`/`20m`. Revisions after merging main need fresh board measurements; predictions
are in [the input guide](../../docs/spi-input.md).
