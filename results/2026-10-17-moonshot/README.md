# 40 MHz: QPSK 9/10 at 33.33 Msym/s

The S3 transmitted QPSK 9/10 at 33.33 Msym/s, roll-off 0.20, 40 MHz occupied, 58.2 Mb/s, for
30 s with the radio keyed inside the shielded box, and met every bank deadline: zero late fills,
zero underruns. The same mode holds 30 s radio off, its samples decode bit-exact in LeanDVB, and
every mode main already runs still passes the regression gate. The margin is thin: the encoder's
mean is 99% of a frame keyed, so this mode is working, not yet qualified with the 15% reserve.
Nothing received the signal, so this shows the transmitter keeping time, not RF quality.

Firmware `51764f0` (exp/moon-final-astra), ESP32-S3 at 240 MHz, 80 MS/s DAC, wide transmit filter
mode selected and read back. Keyed: closed lab Faraday box, placeholder callsign N0CALL,
`dvb 2420 11 33m333 30 4 128` (`keyed.jsonl`). Radio off: `host/check.sh --moonshot` (`check.log`
and records; 37 of 40 pass) and a slim run (`slim.jsonl`).

| QPSK 9/10, 33.33 Msym/s, 30 s | Radio off | Keyed |
|---|---:|---:|
| Status | ESP_OK | ESP_OK |
| Late fills / fatal / underruns | 0 / 0 / 0 | 0 / 0 / 0 |
| Longest fill (bank due every 47,220 cycles) | 42,472 | 41,130 |
| Encoder mean / live max per frame (frame lasts 239,630) | 190,048 / 219,254 | 237,370 / 265,333 |
| Encoder's smallest lead, symbols | 11,520 | 5,120 |
| Free internal RAM with Wi-Fi up | 21,207 B | 28,667 B |

The three failures in `check.log` are the 15% reserve gates (fill at most 40,137, encoder at
most 203,685) and the admissions that depend on them. With the radio keyed, Wi-Fi work on core 0
raises the encoder mean from 190k to 237k cycles per frame; slow frames are absorbed by the queue.

## How it fits

- **Sign windows** (exp/moon-wild-a): QPSK's I and Q are two BPSK streams. Each four-sample output
  vector depends on eight signs per axis, so complete rounded I and Q outputs come from small
  tables and one OR per vector.
- **The mapper writes sign histories directly**, so no separate preparation pass runs per bank
  (round eight), and grouped five-symbol spans keep it at 114k cycles per frame (round nine:
  191k QEMU instructions down to 100k).
- **Bank geometry at 80 MS/s**: lead 128, overlap 512, 15,740 new samples per 47,220 cycles.
- **Roll-off 0.20** with a seven-symbol fractional pulse at 2.4 samples per symbol; the encoder
  BCH/LDPC kernels from exp/moon-enc; tables kept resident so the encoder takes no flash misses.

Twelve rounds across six branches got here: the helper split (rounds 1-5, regressions), the
row queue (exp/moon-wild-b, measured but over budget on core 0), sign windows (adopted), and two
final attempts in parallel, of which this one won. The branches keep the losers.

## Not shown

- No receiver: spectrum, filter flatness at +-20 MHz and MER over RF are still to measure.
- 8PSK at 40 MHz (87.3 Mb/s) does not fit: its tables exceed RAM and its frames last 159,797 cycles.
- The 15% reserve: about 15k cycles per frame on core 0 keyed.
