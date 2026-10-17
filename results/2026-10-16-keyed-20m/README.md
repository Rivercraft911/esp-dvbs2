# Keyed at 20 Msym/s, inside a shielded box

The S3 transmitted 8PSK at 20 Msym/s from its 80 MS/s DAC, 38.7 Mb/s at rate 2/3 and 48.4 Mb/s
at 5/6, for 30 s each with the radio keyed, and met every bank deadline. With the DAC engine
reading banks at 80 MS/s and Wi-Fi up, the longest fill (45.1k cycles) was no slower than in
the radio-off runs (45.3k, results/2026-10-16-20msym). Nothing received the signal, so this
shows the transmit path keeping time, not RF quality.

Firmware `5dbfcea` (main), ESP32-S3 at 240 MHz, board and antenna in the closed lab Faraday box,
placeholder callsign N0CALL. `dvb 2420 <MODCOD> <rate> 30 4 128`; records in `runs.jsonl`. The
30 s include the 20 WPM Morse ID at start and end, which at 80 MS/s sits at +2 MHz.

| Run | DAC | Longest fill | Bank period | Smallest slack | Encoder mean / max | Min lead, symbols | Free internal, Wi-Fi up |
|---|---:|---:|---:|---:|---:|---:|---:|
| 8PSK 2/3, 20 Msym/s | 80 MS/s | 45,092 | 47,220 | 1,842 | 166,233 / 488,765 | 23,684 | 21,211 B |
| 8PSK 5/6, 20 Msym/s | 80 MS/s | 45,068 | 47,220 | 1,869 | 184,201 / 429,293 | 19,749 | 21,691 B |
| 8PSK 2/3, 10 Msym/s | 80 MS/s | 37,425 | 47,220 | 9,513 | 322,657 / 514,989 | **378** | 25,467 B |
| 8PSK 2/3, 10 Msym/s | 40 MS/s | 43,541 | 85,608 | 41,781 | 166,590 / 398,765 | 30,922 | 23,035 B |

Every run: `ESP_OK`, 0 late fills, 0 late switches, 0 fatal, 0 underruns, front-end gain index 0,
free internal RAM with Wi-Fi up above the 20 KB floor.

**10 Msym/s at 80 MS/s is not safe yet.** Its encoder runs the 8 samples/symbol path with the
workspace in PSRAM (results/2026-10-16-next2): 323k cycles per frame on average and up to 515k
with Wi-Fi active, against a 532,656-cycle frame. The encoder's lead fell to 378 symbols, 38 µs
from playing a dummy frame. Use 10 Msym/s at 40 MS/s until that path is fixed.

At 20 Msym/s a 22,194-symbol frame lasts 266,328 cycles, so the encoder uses 62% (2/3) and 69%
(5/6) of core 0 on average; its slowest frames with Wi-Fi running took up to 489k, absorbed by a
lead of at least 19,749 symbols (1.0 ms).

## Not shown

- No receiver. The signal occupies 27 MHz (+-13.5 MHz at roll-off 0.35), wider than the +-10 MHz
  measured for the transmit filter in this bring-up mode (results/2026-10-16-radio), so the band
  edges are attenuated by an unknown amount until a cabled capture measures it.
- The fill's slack is 4% of the bank period.
