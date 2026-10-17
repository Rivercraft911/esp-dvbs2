# Sending files over DVB-S2

`host/filelink.py` is a host-side reference for sending a file (a picture, a log) over the
GCS stream so that whole lost BBFRAMEs do not lose the file. It is independent code and
not a standard wire format (not RaptorQ, RFC 5510, MPE, ULE or GSE). Nothing in the
firmware uses it yet. Results: [results/2026-10-16-filelink](../results/2026-10-16-filelink/README.md).

## Idea

A BBFRAME that fails BCH is simply missing at the receiver, so treat it as an erasure.
Cut the file into equal shards, add repair shards with an erasure code across them, and put
each shard in its own packet. Reed-Solomon over GF(256) recovers the `k` source shards from
any `k` of the `k + m` packets. With one packet per DATA FIELD, any `m` lost frames per block
of `k + m` frames are survivable.

## Packet format

All multibyte integers are big-endian.

| Field | Bytes | Meaning |
|---|---:|---|
| Magic/version | 4 | ASCII `SFL1` |
| File identity | 32 | SHA-256 of the unpadded original file |
| File length | 8 | Original byte count |
| Sequence | 4 | `block × (k + m) + symbol_index`, source shards first |
| `k`, `m` | 1 each | Source and repair counts, each at least 1, sum at most 256 |
| Shard size | 2 | Padded payload length, 1-65535 bytes |
| Shard | variable | Source or repair bytes |
| CRC-32 | 4 | `zlib.crc32` of all preceding packet bytes |

The overhead is 56 bytes, and every packet repeats the manifest, so any packet identifies
the transfer. Files have at least one block; the last source block is zero padded.

The code is a systematic Cauchy matrix `[I; P]` with `P[r, c] = 1 / (r XOR (m + c))` in
GF(256), field polynomial `x^8 + x^4 + x^3 + x^2 + 1` (0x11d), `r = 0..m-1`, `c = 0..k-1`.
Row and column values are disjoint, so every square submatrix is invertible and any `k`
distinct packets suffice. The CRC protects each packet; the SHA-256 checks the reassembled
file. Neither authenticates a sender.

## Into BBFRAMEs

`bbframes()` builds GCS BBFRAMEs with the existing `host/dvbs2.py` adapter: a private SYNC byte
0xb9, UPL and SYNCD 0, byte-aligned DFL. Whole packets share a DATA FIELD but never cross one.
The output uses the same records as LeanDVB's `--fd-bbf` (10-byte BBHEADER + DFL/8 bytes).
The default shard fills one frame at rate 2/3: 5,314 source bytes + 56 = 5,370 bytes.

With the defaults `k = 16`, `m = 4`, any four lost frames per 20-frame block are recovered.
At 8PSK 2/3 and 5 Msym/s a block takes 20 × 22,194 / 5 × 10^6 s = 88.8 ms, and the file
goodput is 9.678 Mb/s × 16/20 × 5,314/5,370 = 7.66 Mb/s (derived, before the last block's
padding). Five losses in one block are not guaranteed to recover.

## Use

```sh
python3 host/filelink.py encode picture.jpg picture.bbf
python3 host/filelink.py recover received.bbf recovered.jpg
python3 host/filelink.py recover received.gcs recovered.jpg --payload   # from decode.py --output
python3 tests/test_filelink.py
python3 tests/test_filelink.py --loopback --receiver /path/to/leandvb
```

Recovery checks BBHEADER CRC and DFL, finds packets by magic and CRC, groups them by file and
sequence, ignores identical duplicates, solves each block, trims padding and checks the
SHA-256. It writes the output only after that check passes.

## Not done

The prototype keeps the whole file in memory and injects losses after decoding, so it says
nothing about receiver reacquisition after an RF outage or real-time throughput. A firmware
sender needs bounded memory and must keep each DATA FIELD intact when it feeds the encoder;
dropping arbitrary input bytes would shift packets across frames and break the loss guarantee.
For receivers that only output MPEG-TS, the same packets would need an IP encapsulation such as
MPE (EN 301 192) or ULE (RFC 4326) on their own PID.
