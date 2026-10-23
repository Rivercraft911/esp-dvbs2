# VCM over the air, keyed into the B200

**Result.** Every VCM mode the transmitter supports decoded from the air with every frame exact,
in order and with the right MODCOD. Five keyed runs, 13,580 frames recovered by LeanDVB, 0 wrong,
0 gaps. Every MODCOD of every pattern came back.

- **1 Msym/s, QPSK 1/2 to 9/10** (MODCOD 4 to 11, one frame each): 591 frames.
- **5 Msym/s, QPSK 1/2 to 9/10**: 3,003 frames.
- **5 Msym/s, 8PSK 3/5 to 9/10** (12 to 17): 4,506 frames.
- **5 Msym/s, mixed, MODCOD 4 to 17 in turn**, the modulation changing between QPSK and 8PSK:
  3,509 frames.
- **Live controls from the laptop over USB** (`examples/usb-send/send.py --schedule`, mixed):
  QPSK 1/2 to 8PSK 2/3 to 8PSK 9/10 to QPSK 9/10 to QPSK 1/2. 1,971 frames, 9,536,190 bytes
  exact and contiguous across all four switches, each switch at the first frame boundary after
  its control. USB delivered 2.74 Mb/s over 27 steady seconds; dummy frames filled the rest.

The only frames missing are at the edges. LeanDVB found the first frame 2 to 15 frames after the
opening ID. The last frame of each synthetic run was cut short by the closing ID, as the window
arithmetic predicts (below), and did not decode. The record counts it as played.

The live VCM controls over SPI are in [ota-spi-input](../2026-10-23-ota-spi-input/README.md).

## Setup

- XIAO ESP32-S3 44:B1:76:B0:E5:8C, firmware 517c641, callsign KJ5SCH, 2420 MHz, gain 0, peak 128.
  Coax with 40 dB of marked attenuation to the B200 (34A6B8E), TX/RX port, receive only, gain
  30 dB.
- Synthetic runs: `vcm 1 PATTERN`, then `dvb 2420 FIRST [5m] 30 0 128`, then `vcm off`. Two
  4.98 s IDs leave 20.04 s of data. The USB run: `send.py send rand40.bin --kind gcs --modcod 4
  --rate 5m --mixed --schedule 0:4,4:13,8:17,12:11,16:4 --key --frequency 2420 --seconds 40`.
  The schedule's times count from USB ready plus the opening ID.
- Continuous sc16 capture of the whole run: 4 MS/s at 1 Msym/s, 10 MS/s at 5 Msym/s. No
  overflow and no lost sample in any run.
- LeanDVB (the build with the corrected short 3/4 table) over each whole capture, with
  `host/decode.py`'s options.

## Checks

- Synthetic payload: each frame must carry the 40-byte station header and then the xorshift32
  stream, regenerated from the state solved from six payload bytes. The next frame must
  continue the stream, and its MODCOD (from LeanDVB's PLS decode) must be the next entry of the
  pattern. Stepping the stream from its seed (2026) gives each frame's number in the run.
- USB: `host/decode.py`'s `live_vcm_match`. The recovered data fields must be one contiguous
  slice of the bytes the sender confirmed, every BBHEADER and PLS must match its frame size,
  the MODCOD runs must follow the controls in order, and each switch must come within one
  old-mode frame of its control's byte offset.

## Runs

| Pattern | Symbol rate | Played | Decoded | Frames decoded | Exact, in order | MODCODs | MER (dB) |
|---|---:|---:|---:|---|---|---|---:|
| QPSK 4..11, hold 1 | 1 Msym/s | 603 | 591 | 11 to 601 | 591 | all 8 | 21.7 |
| QPSK 4..11, hold 1 | 5 Msym/s | 3,011 | 3,003 | 7 to 3,009 | 3,003 | all 8 | 20.0 |
| 8PSK 12..17, hold 1 | 5 Msym/s | 4,515 | 4,506 | 8 to 4,513 | 4,506 | all 6 | 20.2 |
| Mixed 4..17, hold 1 | 5 Msym/s | 3,512 | 3,509 | 2 to 3,510 | 3,509 | all 14 | 19.9 |
| USB live, 4, 13, 17, 11, 4 | 5 Msym/s | 1,986 | 1,971 | 15 to 1,985 | 1,971 | all 5 runs | 19.9 |

- Played is the board's `frames_played`. In each synthetic run the last played frame is the
  one the closing ID cut: at 1 Msym/s, 20.04 s holds 602.1 frames of 33.282 ms, so frames 0 to
  601 are whole and frame 602 is not. At 5 Msym/s the same arithmetic gives 3,010 whole QPSK
  frames, 4,514 whole 8PSK frames, and 3,511 whole mixed frames (250 full cycles of 399,420
  symbols, then 11 frames). In every run the last decoded frame is the last whole one.
- 0 BCH failures between the first and last decoded frame in every run. LDPC corrected 0 to 10
  bits per run. MER is LeanDVB's PLHEADER MER, the median over that stretch.
- Board records: no late bank fill, no dummy frame in the synthetic runs; `"vcm": true` in
  every run; the USB run counted 5 controls and rejected none. The USB sender's own check
  passed (counts, terminal NOP, no protocol errors).

## Records

`runs.jsonl`: one record per keyed run (board completion record, B200 settings and capture
status, LeanDVB report, check; the USB run also has the sender's report), then a recheck of
the 1 Msym/s run from its saved BBFRAMEs that adds the frame numbers.
