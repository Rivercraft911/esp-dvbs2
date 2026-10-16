# Continuous encoding: one core encodes, the other fills the DAC banks

The transmitter no longer loops 8 frames. Core 0 encodes a non-repeating payload stream frame by
frame into a ring in PSRAM while core 1 shapes the symbols into the DAC banks, and both meet
their deadlines with the radio off. LeanDVB recovers a contiguous, bit-exact slice of the stream
from every dump: 38 of 40 frames for QPSK 1/2, 2/3, 3/4 and 9/10, and 198 of 200 frames for
QPSK 1/2 (6.36 Mbit compared), with zero LDPC bit flips. In paced runs that keep the real bank
schedule against the cycle counter, 4.5 million bank fills over 27 minutes all finished in
time, with at least 42,500 cycles (177 us) to spare of the 85,608-cycle bank period. Unless it
was slowed on purpose, the encoder never fell behind: it used 34 to 36% of core 0 and stayed at
least 219 ms ahead. When it was slowed, the fill sent empty frames and the payload stayed
contiguous.
Nothing was transmitted: the commands used never start the radio.

## What ran

Firmware `85d6189` (built from that clean commit), ESP32-S3 rev 0.2 at 240 MHz, ESP-IDF v6.1,
GCC esp-15.2.0_20251204. Only `info`, `dvbdump` and `dvbpace` were sent to the board. Commands,
in `runs.jsonl` with the host time:

- `python3 host/board.py "dvbdump <modcod> <frames> [peak] [decimation] [stall ms]" --save <file>`:
  the samples the DAC would play, every 10th (4 MS/s) or all (40 MS/s), over USB as int16 I/Q.
  Every dump arrived intact (host CRC-32 equals the board's).
- `python3 host/board.py "dvbpace <modcod> <seconds> [peak] [stall ms]"`: the encoder and the
  timed bank fills for that long.
- `python3 host/onair.py <file> --rate 4000000 --scale 512 --modcod <m> --stream <frames>` for
  each 4 MS/s dump (`decodes.jsonl`), and `onair.dump_mer(..., stream=True)` for the 40 MS/s
  dump (`mer.jsonl`). LeanDVB [84c59e1c](https://github.com/pabr/leansdr/tree/84c59e1c7a1a79338d5722d63f28640cc9d350f3)
  with the local macOS/status patch, executable SHA-256 `5482ab74...`, as in
  [the decode results](../2026-10-16-decode/README.md). Python 3.13.2, numpy 2.2.5, scipy
  1.17.1; `host/onair.py` SHA-256 `5733467a...`, `host/decode.py` `896f9ead...`. The dumps
  (11 to 107 MB each) are not in git.

## How it works

- An encoder task on core 0 (priority 5) writes QPSK normal frames with pilots, as phase codes,
  into 8 frame slots in PSRAM (266 KB): frame e goes to slot e % 8 and carries payload bytes
  [e B, (e + 1) B) of one xorshift32 stream seeded with 2026, B = (Kbch - 80) / 8. Its workspace
  (12.4 KB) is in internal RAM.
- The bank fill on core 1 plays the frames on a fixed grid, position p = symbols
  [33282 p, 33282 (p + 1)). The first time a fill reads position p it takes the next finished
  frame, or a filler frame if the encoder is late, and counts an underrun. The filler is the same
  MODCOD carrying an empty BBFRAME (DFL = 0; see below for why not a dummy PLFRAME), so the frame
  grid and the receiver's lock continue and the payload stays contiguous: a late frame is
  delayed, never dropped. The choice is kept, so the bank overlap re-reads the same symbols.
- The fill never blocks. It hands frames back with one counter (`freed`), the encoder announces
  frames with another (`published`); each side reads the other's counter with acquire and writes
  its own with release ordering, which GCC compiles to `memw` around the loads and stores. Both
  cores reach PSRAM through one shared data cache (ESP32-S3 TRM v1.8, section 4.3.3.2, p. 405).
- A frame is released when no later fill can read it: a fill for samples [first, first + count)
  belongs to a bank that starts at or after first + count - 16380, and later fills start later
  still, so symbols below (first + count - 16380) / 40 - 8 are done (8 is the shaper's history).
- `dvbdump` runs `radio_s3_dry_run`, which now also runs on core 1. `dvbpace` runs
  `radio_s3_paced_run`: the timed loop of a transmission, the same source inlined a second time
  with the two register writes removed. Its machine code loads no peripheral address and contains
  no `memw`; the transmit copy loads the DAC trigger 0x60033d64 and the bank ownership register
  0x600c101c (`xtensa-esp32s3-elf-objdump`).
- `tests/test_ring.py` runs `ring.c` on the host against a model of the bank schedule, with a
  simulated encoder at 37%, 125% and in 12-frame bursts, and with two real threads: every symbol
  reads the same at every overlap, no slot is read while written, frames stay in order and every
  filler matches a counted underrun.

## Timing budget

| Quantity | Value | Source |
|---|---:|---|
| Cycles per DAC sample | 240 MHz / 40 MS/s = 6 | radio_s3 |
| Bank switch period | 14268 samples = 85,608 cycles = 356.7 us | 16380 - 64 lead - 2048 tail |
| Fill deadline | the next switch; fatal 1984 samples (11,904 cycles) later | radio_s3 |
| QPSK frame at 1 Msym/s | 33,282 symbols = 33.28 ms = 7.99 M cycles | EN 302 307-1 Table 11 |
| Encoder per frame | 2.7 to 2.9 M cycles, 34 to 36% of core 0 | measured below |
| Encoder lead (predicted) | 7 frames - one encode - release delay = about 220 k symbols | ring geometry |
| PSRAM traffic | fill 1.2 MB/s, encoder about 5 MB/s | estimate; reads measured at 59 MB/s |

The encoder can only refill a slot once the fill has moved past the frame in it, so after the
fill starts a new frame at most 7 frames are ready, and the next one arrives one encode (12 ms)
later. The host model predicts a minimum lead of 219,718 symbols; core 0 may therefore stall for
about 219 ms at 1 Msym/s before the first filler frame goes out.

## The samples

| Dump | MODCOD | Frames | Overlap mismatches | Max fill per bank | Underruns | BBFRAMEs | LDPC flips | Payload |
|---|---|---:|---:|---:|---:|---:|---:|---|
| `dvbdump 4 40` | QPSK 1/2 | 40 | 0 of 7,884,096 | 36,920 | 0 | 38 | 0 | frames 0-37, 152,608 B, match |
| `dvbdump 6 40` | QPSK 2/3 | 40 | 0 of 7,884,096 | 37,432 | 0 | 38 | 0 | frames 0-37, 204,060 B, match |
| `dvbdump 7 40` | QPSK 3/4 | 40 | 0 of 7,884,096 | 38,042 | 0 | 38 | 0 | frames 0-37, 229,558 B, match |
| `dvbdump 11 40` | QPSK 9/10 | 40 | 0 of 7,884,096 | 36,202 | 0 | 38 | 0 | frames 0-37, 276,032 B, match |
| `dvbdump 4 200` | QPSK 1/2 | 200 | 0 of 39,414,144 | 37,019 | 0 | 198 | 0 | frames 0-197, 795,168 B, match |

"Match" means LeanDVB's recovered payload equals the stream's bytes from the start of the frame
shown, bit for bit, with nothing missing, inserted or repeated; the last two frames are still in
the receiver's pipeline when the dump ends. The 8-slot ring wrapped 25 times in the 200-frame
dump. LeanDVB reported zero unsatisfied parity checks before LDPC decoding and no BCH failures
in every frame.

The stream is deterministic: the 40-frame QPSK 1/2 dump is byte for byte the first 40 frames of
the 200-frame dump, and its first 8 frames equal the looping firmware's dump (`35739b5`,
[dry run](../2026-10-16-dryrun/README.md)) except the first 8 symbols, where the shaper's history
before symbol 0 is now zero instead of the loop's last symbols. **MER 47.0 dB** for
`dvbdump 4 2 128 1` (40 MS/s, 66,364 symbols) against the symbols `host/dvbs2.py` encodes from
the stream, the same value as the dry run.

## Timing with the encoder running

| Run | Fills | Late fills | Min slack (cycles) | Max fill per bank | Underruns | Min lead (symbols) | Encoder mean, max (M cycles) | Core 0 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `dvbpace 4 60` | 168,209 | 0 | 43,615 | 41,723 | 0 of 1,803 | 219,361 | 2.88, 2.94 | 36% |
| `dvbpace 4 60` | 168,209 | 0 | 43,739 | 41,601 | 0 of 1,803 | 219,364 | 2.88, 2.94 | 36% |
| `dvbpace 4 60` | 168,209 | 0 | 43,435 | 41,904 | 0 of 1,803 | 219,361 | 2.88, 2.94 | 36% |
| `dvbpace 11 60` | 168,209 | 0 | 43,597 | 41,745 | 0 of 1,803 | 220,074 | 2.71, 2.74 | 34% |
| `dvbpace 11 60` | 168,209 | 0 | 43,709 | 41,629 | 0 of 1,803 | 220,072 | 2.71, 2.74 | 34% |
| `dvbpace 11 60` | 168,209 | 0 | 43,630 | 41,710 | 0 of 1,803 | 220,075 | 2.71, 2.74 | 34% |
| `dvbpace 4 60 128 25` | 168,209 | 0 | 42,537 | 42,804 | 152 of 1,803 | 0 | 2.88, 2.95 | 36% |
| `dvbpace 4 600` | 1,682,086 | 0 | 43,465 | 41,879 | 0 of 18,028 | 219,208 | 2.88, 2.94 | 36% |
| `dvbpace 11 600` | 1,682,086 | 0 | 43,457 | 41,888 | 0 of 18,028 | 220,072 | 2.71, 2.74 | 34% |

`dvbpace` runs the transmission's bank schedule for the given time: 40 MS/s, a switch every
14,268 samples (85,608 cycles), each fill into the real bank memory, with the encoder working on
core 0. Slack is the time left between the end of a fill and its bank switch. Over all runs: 4.54
million fills, 0 late and 0 fatal. The longest fill took 41,904 cycles (49% of the period) with
a normal encoder and 42,804 cycles in the stalled run; the shortest slack was 43,435 and 42,537
cycles. 0.31% of the switches (14,158) landed after their nominal time, at most 746 cycles (124
samples) late, inside the 2048-sample overlap; the keyed radio runs showed the same 0.3% and
746 cycles. The minimum lead agrees with the host model (219,718 symbols for a 37% encoder)
within 0.3%.

Fills are slower here than in the dumps (at most 41.9 k against 36.2 to 38.0 k cycles per bank).
In a dump the ring is full and the encoder idle most of the time; here it encodes 36% of the
time. The difference, up to about 4 k cycles (12%), is most likely the encoder's PSRAM and
internal RAM traffic competing with the fill; it was not separated further. The
encoder's own figure includes payload generation, the shift to phase codes 16 k and core 0's
interrupts; it is about 0.6 M cycles per frame more than `fecbench`'s 2.27 M for QPSK 1/2
([encoder results](../2026-10-16-encoder/README.md)), which encodes into internal RAM. During
dumps it rises to 3.3 to 3.6 M mean because core 0 also serves the USB transfer.

## Underruns on purpose

`stall` makes the encoder sleep after every frame, to check what happens when it falls behind.

- `dvbdump 4 40 128 10 1000`: the dump consumes a frame about every 0.6 s, the encoder makes one
  per second. The board counted 10 underruns. LeanDVB decoded 38 BBFRAMEs: 29 payload frames,
  which are frames 0-28 of the stream with nothing missing, and 9 empty ones at positions 17,
  20, 22, 25, 27, 29, 32, 34 and 37, the receiver locked throughout with zero LDPC flips. The
  dump is identical to the one without a stall up to symbol 106 of position 17, the first symbol
  that carries the filler's different DFL field.
- `dvbpace 4 60 128 25`: the encoder sleeps 25 ms after each 12 ms frame, so it makes a frame
  every 37 ms or so against 33.3 ms needed. 152 of 1803 positions got the filler, the minimum
  lead reached 0, and every fill was still on time (minimum slack 42,537 cycles).

## Why an empty frame and not a dummy PLFRAME

EN 302 307-1 V1.4.1 clause 5.1.5 (p. 19) says that when no data field is ready, the physical
layer shall send a dummy PLFRAME (clause 5.5.1: a PLHEADER and 36 slots of unmodulated carrier,
3330 symbols). The filler here is instead a normal frame of the same MODCOD whose BBFRAME has
DFL = 0. The same clause allows that length (Kbch - 80 >= DFL >= 0), but it is not what the
standard prescribes for this case, and each underrun costs a whole 33.3 ms frame instead of
3.3 ms. It was chosen because every position then has the same length, which keeps the overlap
and release rules simple, and because neither `dvbs2.h` nor `host/dvbs2.py` can make a dummy
PLFRAME yet (the encoder component belongs to other work). Once the encoder can, a dummy-frame
filler with a variable-length grid in `ring.c` is the standard choice. For TS input in the
broadcast profile, clause 5.2.1 requires DFL = Kbch - 80, so empty BBFRAMEs are not allowed
there at all; stuffing would have to be null packets before the encoder, or dummy PLFRAMEs.
LeanDVB accepts the empty frames (above); other receivers were not tried.

## What the paced run does not show

- The DAC does not read the bank memory, so contention between its 160 MB/s reads and the fill
  is not included. The fill writes one 64 KiB bank while the DAC reads the other; whether their
  accesses still meet on the bus is not measured. For comparison, the keyed runs in
  [the radio results](../2026-10-16-radio/README.md) filled banks with the test signals in up
  to 50.7 k cycles (67.6 k for the tag) with 0 late fills and at least 17.7 k cycles of slack;
  the DVB fill here takes at most 42.8 k.
- Wi-Fi is not started. During `dvb` the Wi-Fi task also runs on core 0
  (`CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0`), above the encoder's priority; the encoder lead
  covers about 219 ms of interference.
- Nothing about RF. `dvb` uses this encoder and fill but was not run.

## Open risks for the first on-air run

- Internal RAM: with the encoder allocated, free internal heap is 75.7 KB (largest block 31.7 KB)
  instead of 92.3 KB, and the lowest point during a paced run was 69.5 KB. Wi-Fi's needs at
  `esp_wifi_init` are not measured; if they do not fit, `dvb` fails before keying with an error.
  The `dvb` record now reports `min_free_internal` for the whole command.
- Wi-Fi and PHY activity on core 0 lengthen encode times; `encode_cycles_max`, `underruns` and
  `min_lead_symbols` in the `dvb` record will show it.
- The fill reads PSRAM through the data cache. Any flash write during a transmission would stop
  the cache; nothing in the firmware writes flash during a run (Wi-Fi NVS and PHY calibration
  storage are off).
- `dvb` itself has not run since this change. Its timed loop is the same source as `dvbpace`'s
  (the loop text is unchanged apart from the two register writes, now inside `if (keyed)`), and
  `tests/test_tx_command.py` checks its argument parsing, but the first keyed run is also the
  first test of the refactored `stream()`.
