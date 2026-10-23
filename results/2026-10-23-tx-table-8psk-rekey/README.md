# Keyed table, 8PSK rows after the internal pair tables

A follow-up to [tx-table](../2026-10-23-tx-table/README.md) and
[tx-table-8psk](../2026-10-23-tx-table-8psk/README.md). The method, setup and checks are the
same: 30 s keyed into the B200's TX/RX port at gain 30, four captures, LeanDVB, and every frame
regenerated or matched against its source. Firmware cc2d14b merges exp/tx-8psk fcc3a7f into
exp/tx-clean. That brings in e8ecb9a, which moves the BCH pair tables of 8PSK 8/9 and 9/10 to
internal RAM ([tx-8psk-20m](../2026-10-23-tx-8psk-20m/README.md)).

**Result.** All six re-keyed 8PSK rows decoded every frame exactly: 4,787 frames, 0 wrong.
None sent a dummy frame.

- 8PSK 9/10 at 20 Msym/s now keeps 53.7% encoder spare, up from 3.9%. The fill on core 1
  limits that row now, at 15.7% spare.
- 8PSK 9/10 at 10 Msym/s keeps 77.0% encoder spare.
- The 26.67 Msym/s rows are where tx-table-8psk had them: 20.2%, 16.5% and 21.4% encoder
  spare with a fresh payload.
- With TS input replayed, 8PSK 9/10 at 26.67 Msym/s keeps 2.5% encoder spare. That is below
  the 15% bar, but it runs at full rate: 69.82 Mb/s with no dummies.

| Mode | Payload rate | Exact / decoded | Longest run | MER best / median (dB) | Fill reserve | Encoder reserve | Dummies |
|---|---:|---:|---:|---|---:|---:|---:|
| 8PSK 2/3, 26.67 Msym/s | 51.62 Mb/s | 943 / 943 | 236 | 20.2 / 19.1 | 20.4% | 20.2% | 0 |
| 8PSK 5/6, 26.67 Msym/s | 64.59 Mb/s | 711 / 711 | 237 | 19.0 / 18.3 | 19.8% | 16.5% | 0 |
| 8PSK 9/10, 26.67 Msym/s | 69.82 Mb/s | 948 / 948 | 237 | 18.5 / 17.1 | 20.2% | 21.4% | 0 |
| 8PSK 9/10, 26.67 Msym/s, TS input replayed | 69.82 Mb/s | 949 / 949 | 238 | 18.4 / 17.9 | 18.3% | 2.5% | 0 |
| 8PSK 9/10, 20 Msym/s | 52.37 Mb/s | 707 / 707 | 177 | 17.7 / 16.9 | 15.7% | 53.7% | 0 |
| 8PSK 9/10, 10 Msym/s | 26.18 Mb/s | 529 / 529 | 133 | 20.0 / 18.8 | 50.0% | 77.0% | 0 |

The encoder means are 159.4k, 166.8k, 157.0k and 194.7k cycles at 26.67 Msym/s, against a
199,746-cycle frame. At 20 Msym/s the mean is 123.3k of 266,328 cycles. At 10 Msym/s it is
122.3k of 532,656.

- 8PSK 9/10 at 20 Msym/s had 3.9% encoder spare in tx-table (b778765).
- 8PSK 9/10 at 10 Msym/s was not in that table. W12 measured 254k cycles a frame there before
  e8ecb9a (tx-8psk-20m).
- The TS replay row had 2.7% spare and 4 dummy frames in tx-table-8psk (05510f7).

The 4th capture of 8PSK 5/6 timed out in UHD and decoded nothing. The other 23 captures each
decoded whole. With the transmitter idle before each row, 0.0 to 0.4% of 100 us blocks rose
10 dB above the quietest tenth.

Before keying, all 25 pinned 8PSK dumps on cc2d14b were exact: MODCOD 12 to 17 at 5, 10, 20
and 26.67 Msym/s.

`runs.jsonl`: one `row` summary per row, then its full `run` record (board completion record,
B200 status per capture, LeanDVB report, check).
