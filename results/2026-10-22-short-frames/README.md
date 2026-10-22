# Short FECFRAMEs

QPSK short frames (16,200 bit FEC frames, MODCOD 4..10) are bit exact against `host/dvbs2.py`
in the portable and fast C paths, on the board (PIE kernels) and in decoded waveforms.
Over RF every frame LeanDVB delivered was byte exact (0 wrong frames in 5 captures), but
it delivered only 77% to 94% of the frames sent. A normal-frame control in the same
setup delivered 64%.

Firmware 253de9a (`exp/short-frames`), XIAO-class board (8 MB flash, `sdkconfig.xiao`), app
SHA-256 in `runs.jsonl`. 1 Msym/s, roll-off 0.35, pilots, peak 128, station KJ5SCH.
Payload: xorshift32 seed 2026 with the 40-byte station header; one frame carries
(Kbch - 80) / 8 bytes (869 at MODCOD 4, 1,454 at MODCOD 7, 1,769 at MODCOD 10).

## Commands

```sh
python3 host/board.py "dvbdump 7 128 --short" --save dump.iq
python3 host/decode.py dump.iq --rate 4000000 --symbol-rate 1000000 --scale 512 --modcod 7 \
  --stream 128 --callsign KJ5SCH --receiver "$RX" --short
python3 host/board.py "dvbpace 7 10 --short"
python3 host/board.py "fecbench 7 --short"
python3 host/board.py "dvb 2420 7 40 0 128 --short"   # keyed, 30 dB coax into a B200
```

## Board checks (`dvbdump`, radio off)

* 16-frame dumps of MODCOD 4, 6, 7, 10: CRC-32 of the int16 I/Q stream equals the host build
  of the same source, which checks every DAC word against the portable encoder. All four equal.
* 128-frame dumps of MODCOD 4..10 decoded by LeanDVB: every recovered payload byte matches the
  model, 118 to 120 of 128 frames (acquisition and end of file lose the rest).
* `fecbench` (fast BCH and LDPC alone, one frame, internal RAM, exact-size workspace with
  guard bytes): output CRC-32 equals the model for MODCOD 4..10 short and 7 normal.

## LeanDVB short 3/4 table error

The stock receiver reports about 292 LDPC bit flips and 1 BCH correction per frame on clean
short MODCOD 7 samples. Its table `ldpc_sf_fec34` lists 12 addresses in the first row but sets
`ncols` to 3. Our row equals the standard (and gr-dtv). With `ncols` 12 the same dump decodes with
0 flips and 0 corrections. This is the "short 3/4" entry in `tests/test_dvbs2.py`. Receiver
`sf34-fixed` in `runs.jsonl` is that one-line change.

## Encoder timing (`dvbpace`, 10 s, no underruns, no late fills)

Frame period 8,370 symbols at 1 Msym/s = 8.37 ms = 2,008,800 cycles at 240 MHz. The legacy
n40 path keeps its workspace in PSRAM with nibble-register BCH.

| MODCOD | encoder mean | encoder max | min fill slack | min lead |
|---|---|---|---|---|
| 4 (1/2) | 118,207 cycles (5.9%) | 265,335 (13.2%) | 41,650 cycles | 56,293 symbols |
| 7 (3/4) | 134,941 (6.7%) | 264,600 (13.2%) | 41,713 | 56,085 |
| 10 (8/9) | 150,269 (7.5%) | 274,374 (13.7%) | 41,596 | 56,046 |

Max fill call 43,381 to 43,733 cycles. 1,202 frames per run.

## Fast BCH and LDPC alone (`fecbench`, mean of 99 warm runs, cycles)

Internal-RAM context and workspace, flash or resident BCH table (identical when warm).
First call is cold: BCH 14,386 and LDPC 18,653 at MODCOD 7.

| MODCOD | BCH | LDPC | context bytes |
|---|---|---|---|
| 4 short | 8,575 | 16,478 | 4,672 |
| 5 short | 11,556 | 16,258 | 4,736 |
| 6 short | 12,814 | 14,949 | 5,024 |
| 7 short | 14,083 | 14,244 | 5,312 |
| 8 short | 14,852 | 13,841 | 5,504 |
| 9 short | 15,811 | 13,531 | 5,696 |
| 10 short | 17,056 | 12,883 | 5,984 |
| 7 normal | 57,636 | 33,817 | 16,832 |

## RF (B200, 30 dB coax, 2420 MHz, TX gain 0, 4 MS/s fc32, 50 s captures, no overflows)

Keyed 40 s: two 4.98 s IDs and about 30 s of data (3,575 frames at MODCOD 7). Counts are frames
in the span from the first to the last recovered frame. "Wrong" means the bytes of a delivered
frame differ from the model.

| Run | Receiver | Delivered | Wrong | Missing in span | Longest run | MER median |
|---|---|---|---|---|---|---|
| short 3/4 | sf34-fixed | 3,389 | 0 | 186 of 3,575 | 313 | 19.0 dB |
| short 3/4 | stock | 2,960 | n/a | n/a | n/a | 19.0 dB |
| short 1/2 | stock | 3,023 | 0 | 543 of 3,566 | 91 | 19.2 dB |
| short 3/4, `--phy-solo` | sf34-fixed | 2,764 | 0 | 788 of 3,552 | 198 | 18.5 dB |
| short 3/4, RX gain 50 | sf34-fixed | 3,332 | 0 | 211 of 3,543 | 287 | 19.3 dB |
| normal 1/2 (control) | stock | 632 of about 990 | n/a | n/a | n/a | 18.4 dB |

No capture decoded as one contiguous run, so the stricter "all bytes contiguous" criterion
of `decode.py` fails; the delivered frames are in order and exact. Findings:

* The data interval has no amplitude dropouts (10 ms RMS stays within 8.9 to 10.8 on a 1,000 scale).
* The carrier offset is about -15 kHz and drifts by hundreds of Hz per second.
  De-rotating it offline before LeanDVB did not help.
* Many "failed" frames in the receiver log are wrong-MODCOD header hypotheses (for example 8PSK),
  so the losses look like header or lock losses, not payload errors. The cause is unresolved. The
  same setup loses more normal frames, so it is not specific to short frames.
* Median MER is near 19 dB in every run, set by the transmitter, not the receiver gain.

Captures (1.6 GB each) are not stored. `runs.jsonl` has the board records, the transmit records,
LeanDVB counts and per-frame checks.
