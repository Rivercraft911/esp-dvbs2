# Short frames at 5 Msym/s

QPSK 4..10 and 8PSK 12..16 short FECFRAMEs now run at 5 Msym/s (`5m`, 8 samples per symbol,
40 MS/s DAC). Every stage is bit exact against `host/dvbs2.py`. Over the coax RF path LeanDVB
delivered 24,473 of 26,832 8PSK 8/9 frames (12.64 Mb/s payload on air, 11.5 Mb/s delivered) and
15,360 of 17,946 QPSK 3/4 frames (6.95 Mb/s on air), with 0 wrong frames in every capture. That
needed peak 508. At the requested peak 64 the receiver sees too little signal: QPSK 3/4 delivered
14.7% and 8PSK 8/9 delivered nothing.

XIAO-class board (8 MB flash, `sdkconfig.xiao`), source tree of commit 02119fd on `exp/short-5m`
(the board reports `28b5bfa-dirty` because it was flashed before the commit; the source is the
same). App SHA-256 is in `runs.jsonl`. Roll-off 0.35, pilots, station KJ5SCH. Payload: xorshift32
seed 2026 with the 40-byte station header; a frame carries (Kbch - 80) / 8 bytes: 1,454 at QPSK
3/4 (MODCOD 7) and 1,769 at 8PSK 8/9 (MODCOD 16). Frame length with pilots: 8,370 symbols (QPSK),
5,598 (8PSK = 90 + 5,400 + 3 x 36).

## What changed

* `dvbs2_modes_short` has five more rows: 8PSK 3/5, 2/3, 3/4, 5/6, 8/9 (MODCOD 12..16). They use the
  QPSK short code of the same rate (same Kbch, Nbch, t = 12, q), so LDPC and BCH tables are shared.
  Short 8PSK has no 9/10.
* The 8PSK mapper took its three column offsets (2,700 and 5,400 bytes) from the normal frame. They
  now follow the frame size (675 and 1,350 bytes). MODCOD 12 still reads the columns in reverse.
* The sparse 8PSK mapper (used at 5m, PSRAM-free workspace) puts one 1,440-symbol run after its
  index; the context size now includes that window, and the run starts after the actual index.
* The dummy frame is built unpacked in the second slot; a short 8PSK slot (2,816 B) was smaller
  than that (3,330 B), so slots are at least that long. AddressSanitizer found this on the host.
* `fast.c` lost 8 bytes of IRAM literals, which moved the pinned default renderer
  (`stream_assert.ld`). Two unused literals in `dvbs2_fast_pl` restore it exactly.
* `main.c`: `--short` accepts QPSK 4..10 at 1m or 5m and 8PSK 12..16 at 5m. 10m is refused.
* `host/decode.py --short` accepts 8PSK 12..16.

## Commands

```sh
python3 tests/reference/gen_dvbs2_fast.py            # regenerates fast_tables.h (headers for 8PSK short)
for t in tests/test_*.py; do python3 $t || exit 1; done
python3 host/board.py "dvbdump 13 5m 128 128 1 --short" --save dump.iq
python3 host/decode.py dump.iq --rate 40000000 --symbol-rate 5000000 --scale 512 --modcod 13 \
  --stream 128 --callsign KJ5SCH --receiver "$RX" --short
python3 host/board.py "dvbpace 16 5m 10 128 --short"
python3 host/board.py "dvb 2420 16 5m 40 0 508 --short"   # keyed, 30 dB coax into a B200
```

`$RX` is the fixed LeanDVB build (see below). Argument order of the keyed command is
`dvb MHz MODCOD 5m seconds gain peak --short`.

## Bit exactness

* Host (portable and fast C, `tests/test_dvbs2_c.py::test_short`): 216 GCS and TS frames, QPSK 4..10
  and 8PSK 12..16, pilots on and off, every stage (BB scramble, BCH, LDPC, interleaver and mapping,
  PLFRAME), shifted output addresses, LDPC impulses, packed and complete APIs in exact-size buffers.
* Host stream harness (`tests/test_stream.py`): 16-frame dumps at 5m for the 12 modes are compared
  word by word with the plain encoder, and their CRC-32 values are pinned in
  `tests/data/short_crcs.json` (keys `MODCOD/16/5m`). The 1 Msym/s pins are unchanged.
* Board (`dvbdump MODCOD 5m 16 128 D --short`, D = 2 for QPSK, 1 for 8PSK): the CRC-32 of the int16
  I/Q stream equals the host build of the same source for all 12 modes. The host build runs the
  portable C path, the board runs the PIE kernels (`runs.jsonl`, type `dump16`). Zero underruns.
* 128-frame board dumps at 40 MS/s, decoded by fixed LeanDVB: every recovered payload byte matches
  the model, 0 LDPC bit flips, 0 BCH corrections (acquisition and end of file lose the rest).

| MODCOD | Mode | Frames recovered of 128 | Payload bit errors | MER median |
|---|---|---|---|---|
| 7 | QPSK 3/4 | 121 | 0 | 26.9 dB |
| 10 | QPSK 8/9 | 118 | 0 | 27.0 dB |
| 13 | 8PSK 2/3 | 115 | 0 | 26.8 dB |
| 15 | 8PSK 5/6 | 114 | 0 | 26.9 dB |
| 16 | 8PSK 8/9 | 114 | 0 | 27.1 dB |

Host dumps of 8PSK 12, 13, 14, 15, 16 (128 frames, decoded the same way) recovered 114, 112, 112,
114, 114 frames, all exact with 0 flips.

## Encoder timing (`dvbpace`, 10 s, 5m, peak 128, no underruns, no late fills)

Frame period at 5 Msym/s: QPSK 8,370 symbols = 1.674 ms = 401,760 cycles at 240 MHz; 8PSK 5,598
symbols = 1.1196 ms = 268,704 cycles. "Live max" excludes the first, cold frames.

| MODCOD | encoder mean | live max | max with cold start | min fill slack | max fill call | min lead |
|---|---|---|---|---|---|---|
| 7 (QPSK 3/4) | 50,421 cycles (12.5%) | 65,103 (16.2%) | 149,808 (37.3%) | 46,479 cycles | 32,689 cycles | 4,807 symbols |
| 16 (8PSK 8/9) | 69,456 (25.8%) | 90,240 (33.6%) | 172,455 (64.2%) | 46,482 cycles | 32,977 cycles | 2,022 symbols |

Frames encoded: 5,975 (MODCOD 7) and 8,933 (MODCOD 16) per run, which matches the frame rate.
There are two ring slots at 5m, so the lead is one frame minus the fill-ahead; the 8PSK lead of
2,022 symbols is 0.40 ms. Late fills and late switches: 0.

## LeanDVB

Use `notes/leansdr-fixed` (stock LeanSDR 84c59e1 with the first row of `ldpc_sf_fec34` changed from
ncols 3 to 12, see results/2026-10-22-short-frames). I checked the other short tables with clean
board and host dumps: QPSK 4..10 and 8PSK 12..16 all decode with 0 LDPC flips and 0 BCH corrections,
which exercises the short tables, the 8PSK short deinterleaver (5,400 rows, reversed columns for
3/5) and the 8PSK constellation. The only bug found is the known short 3/4 table. It also hits 8PSK
3/4 short (MODCOD 14), because that mode uses the same table: stock LeanDVB reports 32,256 flips and
112 BCH corrections over 112 frames, the fixed build reports 0 and 0.

## RF

B200 34A6B8E, UHD 4.8.0, receive only, 2420 MHz, fc32 at 10 MS/s (2 samples per symbol), analog
bandwidth 8 MHz, RX gain 30 dB, 46 s captures, no overflows. 30 dB attenuation and coax. Keyed 40 s:
two 4.98 s IDs and about 30 s of data. The capture script is the receive-only UHD worker of
`examples/over-the-air/capture.py` run as a plain script (not stored). Frames "sent" is the
board's `frames_played`. "Delivered" counts frames that equal the model byte for byte; "wrong" counts
frames whose bytes differ from every model frame. Captures (3.7 GB each) are not stored.

| Run | Peak | Sent | Delivered exact | Wrong | Longest run | LeanDVB MER median | MER max |
|---|---|---|---|---|---|---|---|
| 8PSK 8/9 (16) | 64 (requested) | 26,832 | 0 | 0 | 0 | -12.9 dB | 13.6 dB |
| QPSK 3/4 (7) | 64 (requested) | 17,946 | 2,631 (14.7%) | 0 | 25 | -10.7 dB | 13.1 dB |
| 8PSK 8/9 (16) | 508 | 26,832 | 24,473 (91.2%) | 0 | 87 | 20.9 dB | 23.7 dB |
| QPSK 3/4 (7) | 508 | 17,946 | 15,360 (85.6%) | 0 | 320 | 15.1 dB | 23.3 dB |

The 8PSK 8/9 capture at peak 508 holds 43,292,737 exact payload bytes. MER medians below 0 mean
LeanDVB was unlocked for most of the run; the median is over all its MER reports.

Shorter probes (20 s keyed, about 5,998 frames sent, QPSK 3/4, 2420 MHz unless noted), all 0 wrong:

| Peak / gain | Delivered | MER median | MER max |
|---|---|---|---|
| 128 / 0 | 510 (8.5%) | 5.4 dB | 7.1 dB |
| 128 / 8 | 2,092 (34.9%) | 6.4 dB | 7.9 dB |
| 128 / 17, RX gain 20 | 11 (0.2%) | 6.1 dB | 7.7 dB |
| 256 / 0 | 4,619 (77.0%) | 12.5 dB | 14.3 dB |
| 508 / 0 | 5,406 (90.1%) | 20.1 dB | 23.6 dB |
| 64 / 0 at 2446 MHz | 2,424 (40.4%) | 5.8 dB | 14.7 dB |
| 508 / 0 at 2446 MHz | 5,758 (96.0%) | 19.6 dB | 23.0 dB |

Findings:

* Delivery follows the received level. LeanDVB's MER rises about 6 to 7 dB per doubling of the peak
  setting (5.4 dB at 128, 12.5 dB at 256, 20.1 dB at 508). TX gain index 8 added about 1 dB of MER.
  At peak 64 the median 1 ms RMS in the capture is 4.5 on a 1,000 scale against 1.1 with the
  transmitter off, and 29 at peak 508. The wide 6.75 MHz band (about 7 dB more noise than at 1 Msym/s) and the low signal level
  are the likely causes. I did not separate them.
* The 2.4 GHz band in this room is busy. With the transmitter off, 18 to 45% of 1 ms windows at
  seven centres between 2406 and 2446 MHz were more than 1.5 times the median (RX gain 50). Frame
  losses at peak 508 come in runs (longest 87 to 962 frames), which fits interference-driven loss of
  lock, not payload errors: wrong frames are 0 in all 10 captures that delivered anything.
* The in-band to 3.9..4.7 MHz level ratio at the receiver is 17 to 19 dB at peak 508 and 4 to 6 dB at
  peak 64 (it includes the receiver's own filter and noise). I did not measure transmitter regrowth.
* 2446 MHz (occupied band 2442.6 to 2449.4 MHz, inside 2390 to 2450) was run as a probe only.

## Not done

* 10m (sps 4) short frames: `--short` is refused there. The 8PSK n4 path has a different memory layout
  and was not touched.
* VCM and SPI input with short frames.
* RF at the requested peak 64 delivers too little; a higher peak was needed. I did not look for a
  better coupling (less attenuation is not allowed here).
