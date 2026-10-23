# The receiver's TS CRC-8 on PIE

**Result.** The receiver's TS deframer now computes each whole packet's CRC-8 with the
transmitter's PIE column kernel (`dvbs2_pie_crc8_packet`), not a byte table.

In a 60 s live 8PSK 8/9 run at 5 Msym/s with TS input and no pattern check:

| | Before (54b3ca9) | After (a14a400) |
|---|---:|---:|
| Deframer per frame | 38.1k | 28.4k |
| Per packet | 4.05k | 3.02k |
| Core-0 counters, sum | 209.5k (78.0%) | 200.1k (74.5%) |

The frame period is 268,704 cycles. Measured the way ts-link measured it (with the pattern
check, minus its cycles), the deframer went from 31.3k to 23.5k cycles a frame, below
ts-link's 25.6k. Since main's 57.0k before the lazy fingerprint, that is 59% less. All 47,742
frames and 449,231 packets were exact, with 0 CRC-8 errors.

Boards:

- Transmitter 44:B1:76:B0:E5:54, firmware a1ded15, `spiinput quad ts replay`.
- Receiver 44:B1:76:B0:DA:C8.
- 2420 MHz, coax with 30 dB of attenuation.

The method is that of [rx-ts-cost](../2026-10-23-rx-ts-cost/README.md). The "before" column is
its `live60-after` set.

## The change (a14a400)

**The column path.** A whole packet inside a frame takes the kernel when its reads lie in the
frame. The kernel reads 14 aligned 16-byte blocks from align_down(p, 16). Those must start at or
after the BBHEADER and end by the data field's end.

**The byte-table path.** The table stays the reference. It takes three cases:

- packets at a frame's edges
- packets across frames, whose CRC-8 builds up in the carry
- builds without PIE (`DVBS2RX_PIE` or the host test define `RX_TS_COLUMN_MODEL`)

**Memory.** The column table is 1,552 bytes, built from the byte table in `rx_ts_init` with the
same rule as the transmitter's. It sits in the deframer's state, which comes from the run arena.
The low arena's peak rose from 28,208 to 29,760 of 30,720 bytes, and `arena_fits` still holds
for every MODCOD. No heap call was added: `heap_after_boot` stayed at 0.

**Checks:**

- `tests/test_rx_ts.py` runs every deframer test twice: with the byte table, and with a C model
  of the kernel that makes the same reads (`tests/ts_crc_model.c`, now shared with
  `tests/test_ts_crc.py`).
- A new column test places frames so their data field ends at a page with no access. Removing
  the end-of-field bound faults there. The bound before the BBHEADER cannot be tested this way:
  a read before it reaches only the rest of its own 16-byte block.
- The board bench ([tx-pie-bench](../2026-10-23-tx-pie-bench/README.md)) has a new `rx_ts` part
  (3a9417b). It runs `dvbs2_bb_ts` frames of 8PSK 8/9 and QPSK 1/2 short and QPSK 9/10 normal
  through the deframer on the chip, at every start offset and ending on a watched block boundary.
  All 144 frames gave their packets back exact, and a corrupted packet was marked. The debug
  assistant's monitor saw no read outside a frame. That whole bench run had 0 errors; the
  short 8PSK mapper tails of the first bench record are fixed on main.

## Runs

| Set | Receiver firmware | Seconds | Pattern check | Frames exact | Deframer per frame |
|---|---|---:|---|---:|---:|
| `pattern20` | the a14a400 code before its commit (5c2f908-dirty) | 20 | yes | 12,015 of 12,015 | 23.5k (TS 61.2k minus pattern 37.7k) |
| `live60` | a14a400 | 60 | no | 47,742 of 47,742 | 28.4k |
| `bench` | bench 3a9417b on 44:B1:76:B0:E5:8C | | | 144 frames | |

Other counters:

- **Pilot MER (lowest / median):** `pattern20` 23.9 / 29.8 dB, `live60` 26.6 / 29.8 dB.
- **Transmitter:** 0 underruns and 0 late fills.
- **Receiver:** no dropped or failed frames.

These are wall-clock cycle counters that include preemption by the release task, so they are
upper bounds. The release task's 95.6k a frame and the check's 76.1k are unchanged.

`runs.jsonl`, by `set`: the receiver summaries (role `rx`), the transmitter records (role `tx`)
and the bench's `rx_ts` and `all` records.
