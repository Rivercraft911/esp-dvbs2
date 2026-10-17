# Resident 8PSK tables, offline

Source `0f16358`, merged main `f23454f`, prior board image `3cbf37c`.
No board, serial port or radio was accessed. The new image is a candidate,
not an admitted operating point. Raw compact evidence is in `runs.jsonl`;
large logs, traces and disassemblies stay in ignored `notes/` with hashes.

| MODCOD | 13 (2/3) | 15 (5/6) | 17 (9/10) |
|---|---:|---:|---:|
| Model mean, cycles/frame | 166,262 | 171,746 | 147,651 |
| Model maximum of both frame offsets, cycles/frame | 169,375 | 174,859 | 150,764 |
| Estimated Wi-Fi minimum, B | 26,283 | 21,723 | 22,559 |
| Queue pages | 48 | 49 | 44 |
| LeanDVB exact contiguous frames / supplied | 14 / 16 | 14 / 16 | 14 / 16 |

Each model includes the measured main SPI-copy planning floor of 4,823
cycles/frame. Replacing that floor with the prior candidate's approximately
13,600-cycle input mean adds 8,777 cycles. Cache/bus stalls, interrupt delays
and live bookkeeping are not bounded by replay. A frame lasts 199,746 cycles;
its 15% reserve ceiling is 169,784. The model plus publication/wake allowance
clears full core-0 reserve only at 9/10. The all-rate 15% objective remains open.

The pulse table shrinks from 53,248 to 36,864 B without changing any rounding
or sample. All 38 resident pages and the 8,144 B workspace share the original
65,536 B store. Remaining pages use heap. The complete encoder data, mapper
included, use 23,024/24,176/24,320 B. A first page is published before bulk PL
framing, so its full cost does not extend the first publication gap.

The renderer costs 24 model cycles per twelve samples. Its bank estimate is
38,550 cycles: the prior worst 34,926 plus `ceil(15740/12)*(24-22)` and another
1,000 cycles. The ceiling stays 40,137. SRAM contention has changed; this is
an estimate, never a silicon bound.

Heap estimates replace measured `3cbf37c` allocations with actual S3 sizes,
credit the linked 5,424 B heap-start improvement, reserve 64 B for alignment,
and subtract another 1,924 B for a possible Wi-Fi transient in the candidates.
That transient was observed in the prior QPSK pace. Only the board can confirm
free/largest/minimum heap. QPSK 33m333's predicted minimum is 26,451 B.

Main fixes select paired BCH for synthetic 9/10 at 5m and fuse QPSK render/sign
work. QPSK's resident cache shares 2,320 B with the store; its post-wake coverage
is unchanged (`50-4 = 54-8` pages). The prior 17/5m run exceeded its 225,670-cycle
live encoder ceiling at 227,402. The supplied 11/33m333 record was within all
three reported 2% ceilings, but its 18,787 B pace minimum failed the heap gate;
it also exceeded the stricter core-0 reserve calculation. No ceiling is relaxed.

All 65 retained main waveform CRCs and eight fractional QPSK modes pass against
independent references. The three new CRCs are unchanged. LeanDVB reports zero
payload errors, LDPC flips/checks and BCH failures/corrections. Merged-source QEMU
passes 4,320 guarded slices, 36 resident mapper frames, 96 QPSK sign frames and
4,095 fused QPSK blocks. Native tests cover allocation failures, repeated commands,
sequence wraps and physical bank wraps. The keep-going gate's injected failures
are caught. The merged firmware builds with ESP-IDF v6.1. QEMU CCOUNT and native
clocks/heap counters supply no performance or memory evidence.

The main cleanup removed the old benchmark command and historical reports.
The experiment runner keeps the original 2%/heap limits in a small provenance
fixture, uses the current decoder, and replaces the removed board benchmark
with the host stage oracle. It retains every original pace ceiling and adds
no RF commands. See [design and coordinator command](../../docs/lofty.md).
