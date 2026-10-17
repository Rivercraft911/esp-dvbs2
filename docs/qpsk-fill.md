# QPSK block fill

Candidate interface for the encoder engineer: `shaper_qpsk.h`. No `dvbtx.c` changes.

`shaper_qpsk_samples(dst, src, skip, count, table, n)` consumes four QPSK symbols per
byte, oldest in bits 7:6, newest in bits 1:0. Each two-bit code is `phase_k >> 1`,
where phase k is 1, 3, 5, 7. `src[-2..-1]` supplies eight symbols of history.
`skip` is the sample offset (0 through 4*n-1) into the four-symbol block at src[0].
Use n = 3 or 4. Bulk output and table rows are 16-byte aligned; the sample wrapper
handles partial blocks and other output alignments safely. Empty calls touch no buffer.

Blocks start at global symbol indices divisible by four. They must not restart at
frame boundaries: the normal QPSK PLFRAME has 33,282 symbols, or 2 modulo 4.
A window copied from a frame ring must therefore begin at
`4 * floor(first_sample / (4*n)) - 8`, and must include enough complete blocks for
the last requested sample. Pack any missing future symbols in the last block as
phase 1: their taps for the requested earlier samples are zero. Keep the global
block phase through dummy frames and station-ID pauses too.

`shaper_qpsk_build(table,n,level)` takes a caller-owned 16-byte-aligned store and
16..508 DAC LSB peak. Table sizes: 36,864 B at n=3; 49,152 B at n=4. Both reuse
`shaper_store`; its remaining 28,672 B or 16,384 B may hold other immutable data.
No kernel allocation or static data buffer. The pulse is nine symbols, inverse
DAC-hold corrected, roll-off 0.35. New rounding groups require new dump CRCs.

Timing, memory deltas, correctness evidence and coordinator commands will be added
after the host and QEMU checks. No timing result in this document is a board claim.
