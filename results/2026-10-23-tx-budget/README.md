# Transmitter budget, radio off

Where the transmitter's cycles go in every kept mode, measured on the board with the real
xorshift payload. Two families miss their frame budget today: 8PSK at 26.67 Msym/s (all three
code rates, -1.8% to -10.1%) and 8PSK 9/10 at 20 Msym/s (-4.4%). Both insert dummy frames,
and 8PSK 5/6 and 9/10 at those rates aborted on a late bank fill. QPSK at 33.33 Msym/s
keeps 14.9% (9/10) and 17.8% (1/4). With SPI replay input (Gate A), QPSK 9/10 at 33.33
Msym/s fails after 17 frames and 8PSK at 26.67 Msym/s cannot start: the input slots leave
too little internal RAM for Wi-Fi's 20,000 B floor.

Board: XIAO 44:B1:76:B0:E5:8C, firmware c559548, radio off (`dvbpace`), 10 s of payload per
run, peak 128, a board reset before every run. Cycles per frame at 240 MHz, means over every
encoded frame. "Frame" is the frame period; reserve is 1 - encoder mean / frame period. The
bank deadline is 85,608 cycles at 40 MS/s and 47,220 at 80 MS/s. A mode `11 33m333` is
`dvbpace 11 33m333 10 --phy-solo`; the VCM rows first ran `vcm 1 4 7 11` or `vcm mixed`.

## PHY-solo, xorshift payload

| Mode | Frame | Payload | BB | BCH | LDPC | Map | Pages + PL | Encoder mean | Reserve | Wi-Fi up mean | Max fill / deadline | Result |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| `11` | 7987.7k | 72.8k | 147.4k | 107.6k | 47.8k | 346.2k | 18.8k | 741.0k | 90.7% | 740.9k | 42.5k / 85.6k | ok |
| `11 5m` | 1597.5k | 53.5k | 3.5k | 71.3k | 24.1k | 26.4k | 2.5k | 181.7k | 88.6% | 181.6k | 38.2k / 85.6k | ok |
| `11 10m` | 798.8k | 53.5k | 3.4k | 71.3k | 24.2k | 26.4k | 2.5k | 181.6k | 77.3% | 181.6k | 43.9k / 85.6k | ok |
| `4 20m` | 399.4k | 29.8k | 2.4k | 39.0k | 47.0k | 20.0k | 2.6k | 141.0k | 64.7% | 141.0k | 36.8k / 47.2k | ok |
| `11 20m` | 399.4k | 53.5k | 3.9k | 45.0k | 24.4k | 20.5k | 2.6k | 150.1k | 62.4% | 150.0k | 36.8k / 47.2k | ok |
| `4 26m667` | 299.5k | 29.8k | 2.4k | 39.1k | 47.0k | 20.0k | 2.6k | 141.1k | 52.9% | 141.1k | 39.1k / 47.2k | ok |
| `11 26m667` | 299.5k | 53.5k | 3.9k | 45.0k | 24.5k | 20.5k | 2.6k | 150.2k | 49.9% | 150.2k | 39.1k / 47.2k | ok |
| `4 33m333` | 239.6k | 29.8k | 2.4k | 39.2k | 47.5k | 14.1k | 49.7k | 197.0k | 17.8% | 197.7k | 40.5k / 47.2k | ok |
| `11 33m333` | 239.6k | 53.5k | 3.4k | 44.4k | 24.4k | 14.1k | 49.7k | 203.9k | 14.9% | 204.2k | 40.6k / 47.2k | ok |
| `17 5m` | 1065.3k | 53.5k | 49.8k | 75.9k | 41.1k | 82.9k | 1.8k | 305.3k | 71.3% | 304.2k | 38.2k / 85.6k | ok |
| `17 10m` | 532.7k | 53.5k | 32.5k | 71.9k | 41.3k | 76.7k | 1.8k | 278.0k | 47.8% | 276.8k | 43.7k / 85.6k | ok |
| `13 20m` | 266.3k | 39.7k | 2.8k | 52.2k | 36.8k | 22.9k | 1.8k | 156.5k | 41.2% | 156.5k | 41.4k / 47.2k | ok |
| `15 20m` | 266.3k | 49.5k | 7.1k | 65.3k | 32.3k | 29.5k | 1.8k | 185.9k | 30.2% | 185.9k | 41.5k / 47.2k | ok |
| `17 20m` | 266.3k | 53.5k | 32.5k | 72.0k | 41.2k | 76.8k | 1.8k | 278.0k | -4.4% | 276.8k | 40.9k / 47.2k | late fill, aborted after 4.3k frames, 1265 dummies |
| `13 26m667` | 199.7k | 40.5k | 2.8k | 50.1k | 37.4k | 1.5k | 73.0k | 207.1k | -3.7% | 209.2k | 38.7k / 47.2k | 5204 dummies |
| `15 26m667` | 199.7k | 50.2k | 3.2k | 62.4k | 28.6k | 1.4k | 72.4k | 219.9k | -10.1% | 220.0k | 38.0k / 47.2k | late fill, aborted after 5.4k frames, 3858 dummies |
| `17 26m667` | 199.7k | 54.1k | 3.4k | 44.6k | 24.7k | 1.5k | 73.2k | 203.4k | -1.8% | 204.8k | 38.4k / 47.2k | 4739 dummies |
| `7 --short` | 2008.8k | 12.7k | 23.7k | 47.3k | 25.7k | 43.0k | 2.5k | 155.2k | 92.3% | 155.3k | 43.2k / 85.6k | ok |
| `10 5m --short` | 401.8k | 13.4k | 1.6k | 17.5k | 13.4k | 7.1k | 1.0k | 54.4k | 86.5% | 54.4k | 38.9k / 85.6k | ok |
| `16 5m --short` | 268.7k | 13.4k | 5.3k | 29.7k | 15.5k | 15.3k | 0.8k | 80.4k | 70.1% | 80.4k | 38.3k / 85.6k | ok |
| `VCM 1m (4/7/11)` | 7987.7k | 146.9k | 100.0k | 129.5k | 111.8k | 448.7k | 21.5k | 958.6k | 88.0% | 974.3k | 42.5k / 85.6k | ok |
| `VCM 5m mixed` | 1597.5k | 30.2k | 52.4k | 81.9k | 99.0k | 104.4k | 3.9k | 372.0k | 76.7% | 376.3k | 68.5k / 85.6k | ok |

"Wi-Fi up mean" is the same pace with Wi-Fi running (the old default for the queued modes).
It changes the encoder mean by at most 1%; the queued 8PSK modes fail either way. The map
stage of the queued rates (26m667, 33m333) is only the start of mapping; their page loop
(Pages) holds the rest of mapping, PL framing and the descriptor or sign preparation.

## SPI replay input

| Replay input, PHY-solo | Frame | Payload (ingress) | BB (TS) | BCH | LDPC | Map | Pages + PL | Encoder mean | Reserve | Result |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| `4 20m` | 399.4k | 22.9k | 24.8k | 58.5k | 48.3k | 21.9k | 2.6k | 179.2k | 55.1% | ok |
| `11 20m` | 399.4k | 41.2k | 39.6k | 71.3k | 24.2k | 20.0k | 2.6k | 199.3k | 50.1% | ok |
| `4 26m667` | 299.5k | 22.9k | 24.8k | 58.5k | 48.4k | 21.9k | 2.6k | 179.4k | 40.1% | ok |
| `11 26m667` | 299.5k | 41.2k | 39.8k | 71.3k | 24.2k | 20.0k | 2.6k | 199.5k | 33.4% | ok |
| `4 33m333` | 239.6k | 22.9k | 22.8k | 57.3k | 47.4k | 14.1k | 49.7k | 228.5k | 4.7% | ok |
| `11 33m333` | 239.6k | 42.2k | 42.1k | 71.9k | 25.1k | 14.7k | 48.6k | 262.1k | -9.4% | queue empty after 17 frames, failed |
| `13 20m` | 266.3k | 31.5k | 29.9k | 52.3k | 36.8k | 22.9k | 1.8k | 175.5k | 34.1% | ok |
| `15 20m` | 266.3k | 38.4k | 47.8k | 65.2k | 33.9k | 35.6k | 1.8k | 223.0k | 16.3% | ok |
| `17 20m` | 266.3k | 41.5k | 55.0k | 82.9k | 29.3k | 44.7k | 1.8k | 255.6k | 4.0% | ok |


`spiinput quad ts replay` feeds sealed messages through the real parser, CRC-32 and copies,
so the encoder sees TS (CRC-8 per packet in BB) and switches QPSK BCH to the nibble tables.
8PSK 13/15/17 at 26m667 stopped with ESP_ERR_NO_MEM before playback: the eight 1,336-byte
input slots are allocated before Wi-Fi starts, and the heap's low-water mark during Wi-Fi
start fell to 8,763 to 13,135 B, under the 20,000 B floor.

## Prepared frame (fixture), PHY-solo

| Mode | Payload | Encoder mean | Reserve |
|---|---:|---:|---:|
| `4 33m333` | 3.2k | 170.4k | 28.9% |
| `11 33m333` | 5.4k | 155.9k | 35.0% |
| `13 26m667` | 5.3k | 174.5k | 12.7% |
| `15 26m667` | 6.3k | 179.5k | 10.1% |
| `17 26m667` | 5.9k | 159.1k | 20.3% |

The prepared frame copies one stored payload instead of running xorshift: 3k to 6k cycles
instead of 30k to 54k. That gap is why earlier records with the fixture showed reserve.

## Isolated FEC (`fecbench`)

BCH and LDPC on one frame in internal RAM, no stream running (means of 99 runs). Normal
frames: BCH 38.4k (MODCOD 4) to 64.1k (MODCOD 9 and 15), LDPC 23.5k (MODCOD 11, 17) to 46.0k
(MODCOD 4). The byte tables in flash or internal RAM make no difference here, because
nothing else uses the cache.

## Notes

- Memory during Wi-Fi start (`wifimem`): 78,035 B free with no encoder; 29,195 B for QPSK
  9/10 at 33m333; 30,171 / 27,475 / 25,775 B for 8PSK 13 / 15 / 17 at 26m667.
- 8PSK 9/10 at 5m, 10m and 20m reads its BCH, PRBS and mapping tables from flash. The
  encoder takes 60 to 120 flash data misses per stage and 278k cycles a frame at 20m, against
  156k to 186k for 8PSK 2/3 and 5/6, which keep their BCH table internal.
- The tx/rx split (1cdedfa) moved flash data and made the short-frame encoders 10% slower than
  d0a054f (QPSK 3/4 1m: 141k to 157k cycles; 8PSK 8/9 5m: 69k to 78k) for the same reason:
  flash table misses. Their reserve stays 70% to 92%. Every other mode stayed within its
  spread. `split.jsonl` and `presplit.jsonl` hold those paces.

`runs.jsonl` holds every pace record with `tag` default (Wi-Fi as before), phy-solo, replay or
fixture; `fecbench.jsonl` the FEC and Wi-Fi memory records; `split.jsonl` (1cdedfa) and
`presplit.jsonl` (d0a054f) the paces around the reorganization.
