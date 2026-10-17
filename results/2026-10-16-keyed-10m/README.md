# Keyed at 10 Msym/s, inside a shielded box

The S3 transmitted 8PSK 2/3 and 5/6 at 10 Msym/s (19.4 and 24.2 Mb/s) for 30 s each with the
radio keyed, and met every bank deadline with a third of each bank period to spare. The 5 Msym/s
modes and QPSK at 1 Msym/s passed the same way. Nothing received the signal, so this shows the
transmit path running, not RF quality.

## Setup

- Firmware `0b539cb` (main: the build-out and rd-encoder merged, results/2026-10-16-next).
  ESP32-S3 at 240 MHz.
- Board and antenna inside a closed lab Faraday box (rated about 80-100 dB), USB through the
  box's bulkhead. No receiver. Placeholder callsign N0CALL, used only in the closed box.
- `dvb 2420 <MODCOD> <rate> 30 <gain> 128`, records in `runs.jsonl`. Each run includes the
  20 WPM Morse ID at start and end.

## Runs

Pass criteria, set before the runs (from results/2026-10-16-buildout): `ESP_OK`; 0 late fills,
0 fatal deadline misses and 0 underruns; longest fill at most 72,766 and smallest slack at least
12,842 of 85,608 cycles; front-end gain index 0; free internal RAM with Wi-Fi up above 20 KB.

| Run | Longest fill | Smallest slack | Encoder mean / max per frame | Min lead, symbols | Free internal, Wi-Fi up |
|---|---:|---:|---:|---:|---:|
| 8PSK 2/3, 5 Msym/s | 42,597 | 42,739 | 261,354 / 507,063 | 14,073 | 34,371 B |
| 8PSK 5/6, 5 Msym/s | 42,701 | 42,633 | 290,035 / 455,736 | 12,290 | 33,619 B |
| 8PSK 2/3, 10 Msym/s | 56,746 | 28,590 | 261,261 / 389,997 | 27,355 | 54,499 B |
| 8PSK 5/6, 10 Msym/s | 56,976 | 28,356 | 289,943 / 455,460 | 23,788 | 53,155 B |
| QPSK 1/2, 1 Msym/s | 41,084 | 44,250 | 684,567 / 741,153 | 228,634 | 63,991 B |

Every run: `ESP_OK`, 84,105 bank fills, 0 late, 0 fatal, 0 underruns, gain index 0. All pass.

With the DAC engine reading bank memory and Wi-Fi up on core 0, the fills match the radio-off
runs of the same build within 0.4% (results/2026-10-16-next: 56,881-56,929 at 10 Msym/s,
43,017 at 5 Msym/s). Against the first keyed run (results/2026-10-16-keyed-box, 8PSK 2/3 at
5 Msym/s), the longest fill fell from 78,948 to 42,597 cycles and the encoder from 704,720 to
261,354 cycles per frame; free internal RAM with Wi-Fi up is 34.4 KB, against 41.9 KB then.

The encoder's slowest frame at 5 Msym/s (507k) is twice its mean, likely Wi-Fi work on core 0;
the encoder still stayed at least 12,290 symbols (2.5 ms) ahead.

## Not shown

- No receiver: spectrum, MER and decoding over RF wait for 2.4 GHz antennas or a cabled path.
- The transmit filter in this bring-up mode was measured at about +-10 MHz over the air
  (results/2026-10-16-radio). A 10 Msym/s signal occupies 13.5 MHz (+-6.75 MHz), inside that,
  but the filter's shape across the band is unmeasured.
