# Keyed with the faster fill and encoder, inside a shielded box

Main `aa75bab` (results/2026-10-16-next2) transmitted 8PSK 2/3 and 5/6 at 5 and 10 Msym/s and
QPSK at 1 Msym/s for 30 s each with the radio keyed, and met every bank deadline. At 10 Msym/s
the longest fill fell from 56,976 to 43,526 cycles and the encoder from 261k to 166k cycles per
8PSK 2/3 frame against the previous keyed runs (results/2026-10-16-keyed-10m). Free internal RAM
with Wi-Fi up stayed above the 20 KB floor in every run. Nothing received the signal.

Same setup as results/2026-10-16-keyed-10m: closed lab Faraday box, USB through the bulkhead,
placeholder callsign N0CALL, `dvb 2420 <MODCOD> <rate> 30 <gain> 128`. Records in `runs.jsonl`.

| Run | Longest fill | Smallest slack | Encoder mean / max | Min lead, symbols | Free internal, Wi-Fi up |
|---|---:|---:|---:|---:|---:|
| 8PSK 2/3, 5 Msym/s | 37,038 | 48,296 | 328,439 / 594,887 | 10,506 | 23,543 B |
| 8PSK 5/6, 5 Msym/s | 36,969 | 48,366 | 475,633 / 598,295 | 8,723 | 22,275 B |
| 8PSK 2/3, 10 Msym/s | 43,526 | 41,808 | 166,466 / 396,712 | 30,922 | 23,031 B |
| 8PSK 5/6, 10 Msym/s | 43,526 | 41,804 | 184,360 / 426,730 | 30,922 | 21,687 B |
| QPSK 1/2, 1 Msym/s | 41,408 | 43,928 | 681,851 / 742,332 | 228,634 | 53,135 B |

Every run: `ESP_OK`, 84,105 fills, 0 late, 0 fatal, 0 underruns, front-end gain index 0.

The 5 Msym/s encoder regression of results/2026-10-16-next2 shows more with Wi-Fi running:
8PSK 5/6 averages 476k cycles per frame (main before: 290k), and the encoder's lead fell to
8,723 symbols (1.7 ms), against 12,290 before. Deadlines are not at risk (a 5 Msym/s frame lasts
1,065,312 cycles), and a fix is in progress.

## Not shown

- No receiver. RF quality, MER and decoding over the air are still to come.
