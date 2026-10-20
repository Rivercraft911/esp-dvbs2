# S-band transmit checks

One XIAO ESP32-S3 drove a B200 through 40 dB attenuation, with the correction
profile off. DAC rate was 40 MS/s; receive rate was 10 MS/s with 10 MHz bandwidth.

At 2025, 2067, 2110, 2200, 2245, 2290 and 2420 MHz, QPSK 1/2 at
1 Msymbol/s and QPSK 3/4 at 5 Msymbol/s decoded changing payload bytes
exactly. Each nine-capture matrix included three 2420 MHz references:
116 and 651 byte-exact frames in total from 0.5 s captures.

8PSK 2/3 at 5 Msymbol/s passed at these seven carriers. At 2025 and
2067 MHz, 20 dB receive gain failed; 40 dB returned 108 consecutive frames
each. Three 30 s captures returned 4504 frames at 2025 MHz with
QPSK (7.256 Mb/s), and 6754 each at 2025/2067 MHz with 8PSK (9.672 Mb/s).
Two sustained attempts failed: 2025 MHz QPSK at gain 40, and 2245 MHz
8PSK at gain 20 after 461 exact frames. Both causes remain unresolved.

QPSK also decoded 13 frames at each ±125 kHz offset around 2067 and
2245 MHz; one overflow attempt remains invalid.

`runs.jsonl` records settings and hashes. Station IDs
were excluded. These conducted samples do not establish continuous band
coverage, absolute power, range or long-term reliability. IQ files are
excluded.
