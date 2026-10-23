# TS input at 33.33 Msym/s: where the encoder's time goes

**Result.** QPSK 9/10 at 33.33 Msym/s with TS input replayed keeps 15.3% encoder spare, up
from 14.1%. Keyed for 30 s on 3679c73, it decoded 793 of 793 frames exactly, with 15.4%
bank-fill spare and no dummy frames.

The gain comes from copying payload out of the ingress slots with PIE: 4.8k cycles a frame
became 1.9k. Most of what input costs now is the PV v1 wire CRC-32 check itself. It takes
34.3k cycles a frame, 14.3% of the frame, at about 4.7 cycles/byte. That is close to the
instruction floor of the slicing-by-4 loop.

## Profile

`dvbpace 11 33m333 10` after `spiinput quad ts replay`, radio off, 10,018 frames. Cycles per
frame against a 239,630-cycle frame (33,282 symbols at 80 MS/s, 240 MHz). Sets `pace-before`
(cc2d14b) and `pace-after` (752fbb4) in `runs.jsonl`. The after value is the mean of three
paces, which spanned 202,972 to 202,995.

| Stage | cc2d14b | 752fbb4 |
|---|---:|---:|
| Payload input | 41,257 | 38,430 |
| of which slot copy | 4,821 | 1,945 |
| of which PV check (header, CRC-32, TS syncs) | not split | 34,348 |
| BB with TS CRC-8 by columns | 27,823 | 27,788 |
| BCH | 43,932 | 43,959 |
| LDPC | 23,981 | 23,997 |
| Map, two calls | 27,525 | 27,548 |
| Mapper pages | 40,928 | 40,998 |
| **Encoder mean** | **205,729** | **202,990** |
| Reserve | 14.1% | 15.3% |

- GCS input replayed at the same rate: encoder mean 177,497 (25.9% spare). The PV check costs
  the same, 34.0k. BB is 3.1k instead of 27.8k, because there is no per-packet CRC-8.
- BB with TS input is mostly the CRC-8 of about 39 packets, at about 500 cycles each.

## The slot copy

`ingress_read` copied each message's payload into the encoder's frame buffer with `memcpy`,
at 0.66 cycles/byte. The new code copies up to a 16-byte aligned destination, then whole
vectors with `ingress_copy_pie`, then the tail. That kernel is in
`firmware/components/ingress/ingress_pie.S`:

- It uses one unaligned-load setup, then one load, one `EE.SRC.Q.QUP` and one store per
  16 bytes, software-pipelined one block ahead.
- It reads only the 16-byte blocks that hold source bytes. When the source is aligned, it uses
  plain loads, because the extra unaligned block would hold none.

The copy now costs 1,945 cycles for 7,264 bytes, with head, tail and timing included.

Checks:

- The four TS replay dumps keep their CRCs: QPSK 9/10 33m333, QPSK 3/4 10m, short 8PSK 8/9
  5m and 8PSK 2/3 26m667. These ran on the 752fbb4 code, built before the commit (it reported
  4e85f14-dirty).
- The board bench ([tx-pie-bench](../2026-10-23-tx-pie-bench/README.md)) gained an `ingress`
  part, set `bench`. It ran 8 block counts at all 16 source offsets: 128 cases, all equal to
  `memcpy`, with guards intact. Under the debug assistant's read monitor, no read fell outside
  the source's blocks.

## Zero-copy BB, and the CRC

**Zero-copy.** Building BB straight from the slots would save at most the copy that remains,
1.9k cycles a frame (0.8%). The general case needs a copy fallback anyway. A PV message may
carry as little as one TS packet, so a 7,264-byte frame can span more messages than the 8
slots hold. It was not built.

**CRC.** The CRC-32 check reads each byte once. Its compiled loop is 18 instructions per
4-byte word in a zero-overhead loop: a word load, then 4 extracts, 4 address adds and 4 table
loads, then the XORs. Fusing it with the copy or with BB would remove only the loads that are
shared, about 0.1 cycles/byte.

**Next lever, not built.** A PIE CRC-32 could check four messages at once in the four 32-bit
lanes, with `EE.LDXQ.32` doing each table lookup. A rough count gives about 8 instructions per
word, about 2 cycles/byte instead of 4.7. That would save about 20k cycles a frame.

## Keyed

Set `keyed`: `dvb 2420 MODE 30` with TS input replayed, firmware 3679c73. 3679c73 is 752fbb4
plus the spirx driver fix, which does not touch this path. Same method as
[tx-table](../2026-10-23-tx-table/README.md): B200 TX/RX port, gain 30, four captures, every
user packet matched against the replay pattern.

| Mode | Payload rate | Exact / decoded | Longest run | MER best / median (dB) | Fill reserve | Encoder reserve | Dummies |
|---|---:|---:|---:|---|---:|---:|---:|
| QPSK 9/10, roll-off 0.20, 33.33 Msym/s, TS input replayed | 58.20 Mb/s | 793 / 793 | 199 | 17.5 / 16.2 | 15.4% | 15.3% | 0 |
| 8PSK 9/10, 26.67 Msym/s, TS input replayed | 69.82 Mb/s | 946 / 946 | 237 | 18.5 / 18.1 | 16.9% | 3.9% | 0 |

**8PSK 9/10 at 26.67 Msym/s with TS input.** It improved from 2.4% to 3.9% encoder spare (paces:
194,928 to 192,127 cycles of 199,746). It still runs at full rate with no dummies. This row
is a capacity test. No input has delivered 69.8 Mb/s on the board: quad SPI was tested to
46.6 Mb/s ([spi-input](../../docs/spi-input.md)). The 58.2 Mb/s QPSK row is also above that
tested rate.
