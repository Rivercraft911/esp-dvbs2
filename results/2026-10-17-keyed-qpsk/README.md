# Keyed QPSK at 26.67 Msym/s, inside a shielded box

The S3 transmitted QPSK 9/10 at 26.67 Msym/s from its 80 MS/s DAC, 46.6 Mb/s in 36 MHz, for
30 s with the radio keyed, and met every bank deadline with 17% of the bank period to spare.
QPSK 9/10 at 20 Msym/s (34.9 Mb/s) passed the same way. Nothing received the signal, so this
shows the transmitter keeping time, not RF quality.

Firmware `3865c75` (main), board and antenna in the closed lab Faraday box, placeholder callsign
N0CALL, `dvb 2420 11 <rate> 30 4 128`, records in `runs.jsonl`. The 30 s include the 20 WPM
Morse ID at start and end.

| Run | Longest fill (of 47,220) | Smallest slack | Encoder mean / max | Min lead, symbols | Free internal, Wi-Fi up |
|---|---:|---:|---:|---:|---:|
| QPSK 9/10, 26.67 Msym/s | 38,810 | 8,131 | 226,587 / 524,436 | 32,155 | 41,387 B |
| QPSK 9/10, 20 Msym/s (`20mq4`) | 36,620 | 10,318 | 226,682 / 312,494 | 41,208 | 30,579 B |

Both runs: `ESP_OK`, 152,478 bank fills, 0 late fills, 0 late switches, 0 fatal, 0 underruns,
front-end gain index 0. The encoder's slowest frame at 26.67 Msym/s (524k cycles, with Wi-Fi
active, against a 299,538-cycle frame) was absorbed by a lead of at least 32,155 symbols (1.2 ms).

Not shown: the signal is 36 MHz wide and the transmit filter in this bring-up mode measured about
+-10 MHz, so the band edges are cut until the wide filter mode is enabled and measured.
