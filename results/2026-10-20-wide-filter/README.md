# Wide filter RF check

Restoring PBUS(4,2)=32 after CW setup fixes the missing wide-filter selection.
The `85dc467-pbus` image changes only that filter handling and readbacks from main
`85dc467`; encoder and waveform sources are unchanged. Narrow mode explicitly selects 0.

XIAO S3 → 40 dB attenuation → receive-only B200 at 2420 MHz; TX gain 0, peak 48,
RX gain 40 dB, profile off. Each native 50 MS/s capture contains 2²¹ samples
(41.94304 ms), taken inside a 20 s run with KJ5SCH Morse IDs at both ends.

| Mode | Msym/s | Payload | Exact frames | Receiver |
|---|---:|---|---:|---|
| 8PSK 9/10 | 26.67 | prepared, repeated | 47 | stock |
| 8PSK 5/6 | 26.67 | prepared, repeated | 48 | stock |
| QPSK 9/10, α 0.20 | 33.33 | prepared, repeated | 40 | stock |
| 8PSK 2/3 | 26.67 | prepared, repeated | 48 | stock |
| 8PSK 5/6 | 20 | changing | 34 | stock |
| QPSK 9/10 | 26.67 | changing | 32 | stock |
| 8PSK 2/3 | 20 | changing | 24 | soft LDPC |
| 8PSK 5/6 | 10 | changing | 15 | soft LDPC |

Stock decoding missed one interior frame each at 8PSK 5/6, 10 Msym/s and
8PSK 2/3, 20 Msym/s; fixed soft LDPC recovered the missing frames from the same IQ.
The main 85 wide fixtures decoded zero frames before this fix. Original failures,
parity residuals and all failed gates remain in `runs.jsonl`. QPSK 26.67 Msym/s
decoded 32 exact frames, but both 20 s runs reported two TX underruns, failing
whole-run health.

These short captures prove exact payload recovery, not sustained wideband video.
The B200 captured each channel in one shot; receiver runs above 25 Msym/s
interpolate saved IQ by two.
