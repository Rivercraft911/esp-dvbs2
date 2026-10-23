# The TS deframer's cost, and the duplicate fingerprint

**Result.** The duplicate-packet fingerprint added in 8dd02c7 made the receiver's TS deframer
cost twice as much. It hashed every packet with payload, at 3.1k cycles a packet. It is now
computed only when it can matter (54b3ca9).

At 8PSK 8/9, 5 Msym/s, with TS input, two 60 s live runs without the pattern check:

| | Deframer per frame | Per packet | Core-0 counters, sum |
|---|---:|---:|---:|
| Before | 63.0k | 6.69k | 235.0k (87.5%) |
| After | 38.1k | 4.05k | 209.5k (78.0%) |

The frame period is 268,704 cycles. Measured the way ts-link measured it (with the pattern
check, minus its cycles), the deframer went from 57.0k to 31.3k a frame. ts-link had 25.6k.
Every frame was exact and every packet was accounted for, with the same duplicate semantics as
before.

Boards: transmitter 44:B1:76:B0:E5:54 (a1ded15, `spiinput quad ts replay`), receiver
44:B1:76:B0:DA:C8, at 2420 MHz over coax and 30 dB. The method is that of
[rx-long](../2026-10-23-rx-long/README.md).

## Where the cycles went

These come from a probe build only, never committed. It wrapped cycle counters around the
deframer's stages, on top of main's deframer and then on top of the lazy one. 20 s live runs,
in cycles per packet:

| Stage | Main (e218be2) | Lazy |
|---|---:|---:|
| CRC-8 of the packet (byte table) | 2,168 | 2,270 |
| Continuity check | 3,231 | 191 |
| of which the fingerprint | 3,099 | 208, amortized |
| Emit (pattern check, no egress) | 3,517 | 3,993 |

Main's fingerprint was FNV-1a over bytes 1 to 187, two bytes at a time. It cost 3.1k of the
deframer's 6.06k cycles a packet. That is the whole difference from ts-link: about 29k of 57.0k
a frame.

Putting the deframer's packet path in IRAM changed nothing: 31.3k a frame either way, set
`lazy-iram`, at 1.5 KB of internal RAM. The deframer keeps only its CRC-8 loop in IRAM, as
before.

These counters are wall-clock cycles at 240 MHz. They include preemption by the release task,
which runs a third of the time at a higher priority. So they are upper bounds and overlap.

How the work divides between counters depends on what else runs in the window. Without the
pattern check, the deframer's window absorbs preemptions the pattern window took before. That
is why the no-pattern after-run reads 38.1k a frame against 31.3k in the pattern runs.

## The change

The fingerprint answers one question: whether a packet that repeats its PID's counter has the
same bytes as the last packet with payload, PCR aside (ISO/IEC 13818-1 2.4.3.3). So it is now
taken only at three points:

- **A repeated counter.** When a packet repeats the counter, both packets are hashed: the new one
  and the last one.
- **The frame's end.** For each PID, the last payload packet is hashed if it is still pending.
  The frame's buffer goes back to the receiver after this.
- **The carried packet.** The packet carried across frames is hashed when it is delivered at a
  restart or at the end. Its buffer is refilled after that.

Until then each PID keeps a pointer to its last payload packet. A packet without payload drops
the pointer, because the old fingerprint is then never asked for.

With few PIDs that is about one fingerprint per frame instead of one per packet. The state grows
by 64 bytes (a pointer per tracked PID). The duplicate semantics are unchanged:

- one duplicate allowed, with a new PCR permitted
- a third copy, other bytes, or a payload packet reusing an adaptation-only packet's counter
  are errors

`tests/test_rx_ts.py` changes:

- The harness now overwrites each frame's buffer as soon as the deframer returns, as the
  receiver reuses its slot.
- The continuity test runs with fields of 564, 433, 250 and 100 bytes. An original and its
  duplicate then fall in one field, in two, or one of them is the carried packet.
- A lazy version without the settling at frame end fails this test.
- 8dd02c7's parent, the lenient code that took any repeated counter as a duplicate, still fails
  it (2 continuity errors instead of 5).

`host/rx.py --no-pattern` runs TS without the receiver's packet pattern check, so the egress
budget can be measured as it will run. The replay's continuity jumps are then counted from the
packets handed on (`ts_wraps`). It matched the pattern check's own count in every pattern run
here and in rx-long.

## Runs

| Set | Receiver firmware | Seconds | Pattern check | Frames exact | Deframer per frame | Pilot MER lowest / median |
|---|---|---:|---|---:|---:|---|
| `probe-before` | main's deframer plus probes (e218be2-dirty) | 20 | yes | 12,036 of 12,036 | 57.0k | 27.0 / 29.9 dB |
| `probe-lazy` | lazy plus probes (e218be2-dirty) | 20 | yes | 12,008 of 12,008 | 30.8k | 21.3 / 29.9 dB |
| `lazy` | lazy, before its commit (e218be2-dirty) | 20 | yes | 12,008 of 12,008 | 31.3k | 27.2 / 29.8 dB |
| `lazy-iram` | lazy with the packet path in IRAM (e218be2-dirty) | 20 | yes | 12,003 of 12,003 | 31.3k | 27.3 / 29.9 dB |
| `live60-before` | main's deframer on 54b3ca9 (54b3ca9-dirty) | 60 | no | 47,725 of 47,725 | 63.0k | 22.6 / 30.0 dB |
| `live60-after` | 54b3ca9 | 60 | no | 47,742 of 47,742 | 38.1k | 26.4 / 29.9 dB |

- No run lost or failed a frame, or had a CRC-8 or continuity error beyond the replay's loop
  jumps.
- Every run had `heap_after_boot` at 0. The transmitter had 0 underruns and 0 late fills.
- In the 20 s runs, the deframer per frame is the TS counter minus the pattern check.

## Next lever, not built

The deframer's CRC-8 of each packet is now its largest part: 2.27k cycles a packet in the probe
run, from a byte table. The transmitter computes the same CRC by columns on PIE
(`dvbs2_pie_crc8_packet`, ts_pie.S). There it takes 496 cycles for 176 bytes
([tx-pie-bench](../2026-10-23-tx-pie-bench/README.md) checks it on the board).

Used here for packets whose aligned 224-byte read lies inside the data field, it would save an
estimated 1k to 1.5k cycles a packet, 9k to 14k a frame. The cost is 1,552 bytes of column table
in internal RAM. That is an estimate from the transmitter's measurement; it has not been run
here.

Records: `runs.jsonl`, by `set`, with the receiver summary (role `rx`) and the transmitter
records (role `tx`).
