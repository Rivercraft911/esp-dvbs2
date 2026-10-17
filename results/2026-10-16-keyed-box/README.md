# First keyed DVB-S2 run, inside a shielded box

The S3 transmitted 8PSK 2/3 at 5 Msym/s (the 9.68 Mb/s video mode) for 30 s with its radio
keyed, and met every bank deadline. Nothing received the signal, so this shows the
transmit path running, not RF quality.

## Setup

- ESP32-S3 with its antenna inside a closed lab Faraday box (rated about 80-100 dB), USB out
  through the box's bulkhead. No receiver; the E200 was not used.
- Station ID: the placeholder callsign N0CALL, used only while the box is closed. The firmware
  refuses to key without a stored callsign and sends a 20 WPM Morse ID at the start and end.
- Command `dvb 2420 13 5m 30 4 128`: 2420 MHz, MODCOD 13, 5 Msym/s, 30 s, gain 4, peak 128.
  Records in `runs.jsonl`.

## Runs

1. Firmware `e4405ff`, `dvb ... 10 ...`: refused before keying, "duration too short for
   start/end ID". The two IDs take 2.6 s each, so a run must leave room for them.
2. Firmware `e4405ff`, 30 s: `ESP_ERR_NO_MEM` during Wi-Fi start-up, before keying (0 fills).
   The lowest free internal RAM reached 8,574 bytes. The fast encoder's context and station ID
   had used up the margin the stream run reported.
3. Firmware `1ac2665-dirty`, the same tree plus the Wi-Fi memory settings committed as
   `81da950` (2 static RX buffers, no AMPDU, Wi-Fi code in flash, Wi-Fi allocations in PSRAM).
   Idle free internal RAM rose from 69.0 to 86.8 KB. 30 s run: `ESP_OK`.

| Run 3 | Value |
|---|---:|
| Samples played | 1,200,000,002 (30 s at 40 MS/s), including 208,800,000 of Morse ID |
| Bank fills / late / fatal | 84,105 / 0 / 0 |
| Late bank switches | 260, at most 741 cycles late, inside the 2,048-sample overlap |
| Longest fill, smallest slack | 78,948 and 6,390 of 85,608 cycles |
| Encoder per frame, mean / max | 704,720 / 727,951 cycles (66% of core 0) |
| Frames encoded / played, underruns | 4,414 / 4,407, 0 |
| Lowest free internal RAM | 41,895 bytes |

With the DAC engine reading bank memory, the longest fill (78.9k cycles) is within the
run-to-run spread of the radio-off runs (79.3k-80.2k, results/2026-10-16-8psk-board and
-integrate), so the engine's reads cost nothing measurable here. The margin is still thin:
7% of the deadline.

## Not shown

- No receiver: spectrum, MER and decode over RF are next, once 2.4 GHz antennas arrive or with
  a cabled path.
- Moving Wi-Fi's code to flash might add cache contention during Wi-Fi activity; in NULL mode
  there was none measurable in this run.
