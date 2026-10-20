# Live RF check

CM5 → 16 MHz PV-SPI → XIAO ESP32-S3 → attenuators → B200 → LeanDVB.
2420 MHz, 5 Msymbol/s 8PSK 2/3, pilots, roll-off 0.35; 9 Mb/s TS.
Firmware `42790c2` adds an optional profile measured only on board
`44b176b0e58c`, gain 0, 40 MS/s: `rfprofile bench2420`; default is `off`.

Carrier leakage fell about 10 dB. The corrected 60 dB attenuation / 40 dB RX
run still had 18 TS gaps in 112 s. A 40 dB / 20 dB run had none in 111 s.
This short result does not identify the interference source or establish range.

A saved RF burst failed the same three BCH frames with and without a 1 kHz
carrier notch. RRC 40/60 and LDPC BF 400 also returned identical TS bytes.
The separate 120 s TX-off capture had no sample gaps and no persistent +1 MHz
line; a centered residual remained. Raw IQ and plots stay in local output.

See `runs.jsonl`. AU/s counts recovered encoded frames, not display cadence.
