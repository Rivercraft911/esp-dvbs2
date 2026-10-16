# DVB-S2 from the chip, decoded without the radio

The board encodes DVB-S2 frames, shapes them and fills the DAC banks exactly as it would for
transmission, and LeanDVB decodes the result bit for bit. Four QPSK code rates and three drive
levels all decode with zero LDPC bit flips and zero BCH failures, and the samples match the
independent Python encoder at 47.0 dB MER. Nothing was transmitted: `dvbdump` never starts the
radio.

## What ran

Firmware `35739b5`, ESP32-S3 at 240 MHz. `python3 host/board.py "dvbdump <modcod> <frames>
<peak> <decimation>" --save <file> --out runs.jsonl`, then `python3 host/onair.py <file> --rate
4000000 --scale 512 --modcod <modcod>` for each decimated dump (`decodes.jsonl`), and
`onair.dump_mer()` for the full-rate dump (`mer.jsonl`).

- The board encodes 8 normal QPSK frames with pilots into PSRAM (C encoder, GCS, payload
  xorshift32 from 2026) and loops them. One symbol is 40 DAC samples, 1 Msym/s at 40 MS/s. The
  pulse is the roll-off 0.35 table (the roll-off the BBHEADER signals), scaled so the worst-case
  |I + jQ| is the peak level.
- `radio_s3_dry_run()` calls the fill in the same bank sequence as a transmission, into the same
  reserved bank memory. At each of the bank switches it compares the 2112 words both banks must
  share, then sends the samples the DAC would play, every 10th (4 MS/s) or all (40 MS/s), over
  USB as int16 I/Q.
- Receiver: LeanDVB [84c59e1c](https://github.com/pabr/leansdr/tree/84c59e1c7a1a79338d5722d63f28640cc9d350f3)
  with the local macOS/status patch, executable SHA-256 `5482ab74...` (as in
  [the decode results](../2026-10-16-decode/README.md)), `--roll-off 0.35`. Python 3.13.2, numpy
  2.2.5, scipy 1.17.1; `host/onair.py` SHA-256 `9503ccb0...`.

## Results

Every dump arrived intact (the host's CRC-32 of the received bytes equals the board's) and every
bank overlap matched: 0 mismatches in 3,943,104 compared words per 20-frame dump.

| Dump | MODCOD | Peak | rms per branch | BBFRAMEs | LDPC flips | BCH failures | Loop payload |
|---|---|---:|---:|---:|---:|---:|---|
| `dvbdump 4 20 128 10` | QPSK 1/2 | 128 | 58.3 | 18 | 0 | 0 | 32,128 B, match |
| `dvbdump 6 20 128 10` | QPSK 2/3 | 128 | 58.3 | 18 | 0 | 0 | 42,960 B, match |
| `dvbdump 7 20 128 10` | QPSK 3/4 | 128 | 58.3 | 18 | 0 | 0 | 48,328 B, match |
| `dvbdump 11 20 128 10` | QPSK 9/10 | 128 | 58.3 | 18 | 0 | 0 | 58,112 B, match |
| `dvbdump 4 20 64 10` | QPSK 1/2 | 64 | 29.1 | 18 | 0 | 0 | 32,128 B, match |
| `dvbdump 4 20 508 10` | QPSK 1/2 | 508 | 231.5 | 18 | 0 | 0 | 32,128 B, match |

"Match" means the whole 8-frame loop payload appears, from its first byte, in LeanDVB's
recovered stream. LeanDVB also reported zero unsatisfied parity checks before LDPC decoding in
every frame, so the hard decisions were already error free.

**MER 47.0 dB** for `dvbdump 4 2 128 1` (40 MS/s, 66,364 symbols) against the symbols that
`host/dvbs2.py` encodes from the same payload, through an RRC matched filter (roll-off 0.35,
+-16 symbols) with the best delay in eighths of a sample and a fitted complex gain. The same
code gives 46.5 dB for the Python model of the full-scale table, so the scaled table and the
fill add nothing measurable. The best delay is 178.5 samples: the 360-tap pulse is centred
between samples.

The 4 MS/s dumps measure about 35.7 dB with the same code. Taking every 10th sample with no
filter folds the table's out-of-band floor from 40 MHz into 4 MHz. That is a property of the
dump, not of the signal, and it does not affect decoding.

## Timing on the board

- Fill: at most 43,095 cycles per bank (180 µs of the 356.7 µs bank period, 50% of one core).
  This includes copying symbols from PSRAM and the partial symbols at bank edges. The dry run
  does not run the DAC at the same time, so memory contention with the engine is not included;
  the earlier PIE kernel alone took about 28,700 cycles per bank.
- Encoding into PSRAM: 2.83 to 2.98 M cycles per frame (12.4 ms per 33.3 ms QPSK frame at
  1 Msym/s, 37% of one core).

## Not shown

- No RF. The on-air `dvb` command uses the same fill and waits for a cabled or identified test.
- The frames repeat every 8; continuous encoding while transmitting is not built yet.
- 8PSK and 5 Msym/s are not wired into `dvb` yet (the 8 samples/symbol kernel exists).
