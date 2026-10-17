# 20 Msym/s in real time, radio off

At 80 MS/s the S3 now fills every bank on time for 8PSK at 20 Msym/s: 38.7 Mb/s at rate 2/3
and 48.4 Mb/s at 5/6. Two 30 s paced runs each made 205,541 bank fills with zero late fills,
zero late switches and zero underruns, the longest fill 45.3k cycles against the 47,220-cycle
bank. The margin is thin (1.6k cycles, 3.4%). The encoder needs 167k and 185k cycles per frame,
63% and 70% of core 0. Nothing was transmitted.

Firmware `4731a47` (exp/max-opus: main `aa75bab` plus the 80 MS/s bank geometry and the
rate-aware station ID; docs/bank-geometry.md), ESP32-S3 at 240 MHz. Records: `runs.jsonl` and
`dumps.jsonl` from a clean rerun after a reset, `fec.jsonl` and `id.jsonl` from the first pass.
In the first pass the dumps were run without `--save`, which left the following replies one
command out of step; those records are not used.

## What changed for 80 MS/s

- Bank geometry at 80 MS/s: lead 128 and overlap 512 samples instead of 64 and 2,048, so each
  bank carries 15,740 new samples and is due every 47,220 cycles instead of 42,804. The old
  values came from esp-sdr-zephyr's sleep-based scheduler and were never measured here. 40 MS/s
  is unchanged.
- Interrupts stay masked through the wait before each bank switch. The 0.3% of switches that
  used to land late were the 1 kHz FreeRTOS tick on the fill core.
- The CW station ID keeps 20 WPM at 80 MS/s; its tone sits at +2 MHz there.
- `dvbpace`, `dvbdump` and `dvb` accept `20m` (20 Msym/s, 4 samples/symbol) and `10m80`
  (10 Msym/s, 8 samples/symbol), both at 80 MS/s.

## Results

| `dvbpace`, 30 s | DAC | Longest fill | Bank period | Smallest slack | Encoder mean / live max | Min lead, symbols |
|---|---:|---:|---:|---:|---:|---:|
| 8PSK 2/3, 20 Msym/s | 80 MS/s | 45,343 | 47,220 | 1,608 | 167,031 / 170,104 | 27,999 |
| 8PSK 5/6, 20 Msym/s | 80 MS/s | 45,318 | 47,220 | 1,629 | 185,023 / 186,199 | 24,714 |
| 8PSK 2/3, 10 Msym/s | 80 MS/s | 37,760 | 47,220 | 9,191 | 314,852 / 323,433 | 7,852 |
| 8PSK 2/3, 10 Msym/s | 40 MS/s | 43,798 | 85,608 | 41,538 | 167,242 / 171,336 | 33,685 |

Every run: `ESP_OK`, 0 late fills, 0 late switches, 0 fatal, 0 underruns. Pass criteria set
before the runs: 20 Msym/s longest fill at most 46,000 and slack at least 1,200; 10 Msym/s at
80 MS/s longest fill at most 40,000. All pass.

The 10 Msym/s mode at 80 MS/s runs the 8 samples/symbol encoder path, which keeps its workspace
in PSRAM and carries the 5 Msym/s regression of results/2026-10-16-next2 (315k cycles); its
encoder lead dropped to 7,852 symbols (0.8 ms). The fix for that path is in progress.

## Correctness

`fecbench pie` 140 of 140 CRCs. `iddump N0CALL` at 40 and 80 MS/s both give CRC 1965033598 and
decode as N0CALL at 20 WPM. All eight dumps match: the five references (QPSK 1435641601; 8PSK
2/3 and 5/6 at 5 and 10 Msym/s 1937985440, 1163322913, 474263962, 1757765864), and the 80 MS/s
modes, which play the same samples at twice the rate: `13 20m` 474263962, `15 20m` 1757765864,
`13 10m80` 1937985440. Zero overlap mismatches.

## Not shown

- No RF. The DAC engine reading banks at 80 MS/s, and the transmit filter at 27 MHz occupied
  bandwidth (+-13.5 MHz against the +-10 MHz measured in this bring-up mode), are untested.
- 3.4% slack is thin; the 6-symbol pulse (2.30 cycles/sample at 4 sps, measured) and a cheaper
  overlap copy are the next sources of margin.
