"""Small file link over DVB-S2 GCS. See docs/file-link.md for the wire format.

Independent systematic Cauchy Reed-Solomon erasure code over GF(256), polynomial
0x11d. This is an application protocol, not RaptorQ, GSE or MPE.
Input/output .bbf files use LeanDVB --fd-bbf records (10-byte header + DATA FIELD).
The prototype buffers a file in host memory; it is not a timed flight sender.
"""
import argparse
from functools import lru_cache
import hashlib
from pathlib import Path
import struct
import zlib


MAGIC = b"SFL1"
HEADER = struct.Struct(">4s32sQIBBH")  # magic, SHA-256, length, sequence, k, m, shard bytes
OVERHEAD = HEADER.size + 4             # CRC-32 covers header and padded shard


def _tables():
    exp, log, value = [0] * 510, [0] * 256, 1
    for i in range(255):
        exp[i], log[value] = value, i
        value <<= 1
        if value & 256:
            value ^= 0x11d
    exp[255:] = exp[:255]
    return exp, log


EXP, LOG = _tables()


def mul(a, b):
    return EXP[LOG[a] + LOG[b]] if a and b else 0


def inv(a):
    if not a:
        raise ValueError("singular erasure matrix")
    return EXP[255 - LOG[a]]


def _params(k, m, shard_size):
    if not (1 <= k <= 255 and 1 <= m <= 255 and k + m <= 256):
        raise ValueError("require k >= 1, m >= 1, k + m <= 256")
    if not 1 <= shard_size <= 65535:
        raise ValueError("shard size must be 1..65535 bytes")


def _row(index, k, m):
    # Disjoint sets y=0..m-1, x=m..m+k-1 make every Cauchy minor invertible.
    if index < k:
        return [int(i == index) for i in range(k)]
    return [inv((index - k) ^ (m + i)) for i in range(k)]


@lru_cache(maxsize=256)
def _scale(coefficient):
    return bytes(mul(coefficient, i) for i in range(256))


def _combine(coefficients, shards):
    result = 0
    for coefficient, shard in zip(coefficients, shards):
        if coefficient:
            result ^= int.from_bytes(shard.translate(_scale(coefficient)), "big")
    return result.to_bytes(len(shards[0]), "big")


def _inverse(matrix):
    n = len(matrix)
    rows = [list(row) + [int(i == j) for j in range(n)]
            for i, row in enumerate(matrix)]
    for col in range(n):
        pivot = next((i for i in range(col, n) if rows[i][col]), None)
        if pivot is None:
            raise ValueError("singular erasure matrix")
        rows[col], rows[pivot] = rows[pivot], rows[col]
        scale = inv(rows[col][col])
        rows[col] = [mul(v, scale) for v in rows[col]]
        for i in range(n):
            if i != col and rows[i][col]:
                scale = rows[i][col]
                rows[i] = [a ^ mul(scale, b) for a, b in zip(rows[i], rows[col])]
    return [row[n:] for row in rows]


def encode_packets(data, k=16, m=4, shard_size=1024):
    """Yield k source then m repair packets per block, including for an empty file.

    Sequence = block * (k+m) + encoding-symbol index. Repeat metadata in every
    CRC-protected packet, so losing packet zero does not lose the manifest.
    """
    _params(k, m, shard_size)
    digest = hashlib.sha256(data).digest()
    blocks = max(1, (len(data) + k * shard_size - 1) // (k * shard_size))
    if blocks * (k + m) > 2**32:
        raise ValueError("file exceeds sequence space")
    for block in range(blocks):
        start = block * k * shard_size
        padded = data[start:start + k * shard_size].ljust(k * shard_size, b"\0")
        shards = [padded[i * shard_size:(i + 1) * shard_size] for i in range(k)]
        for index in range(k + m):
            payload = shards[index] if index < k else _combine(_row(index, k, m), shards)
            packet = HEADER.pack(MAGIC, digest, len(data), block * (k + m) + index,
                                 k, m, shard_size) + payload
            yield packet + struct.pack(">I", zlib.crc32(packet))


def packets_from_stream(raw):
    """Scan concatenated GCS data; a CRC failure becomes an erasure, then resync.

    This also accepts the flattened GCS output of host/decode.py. Packets must
    never cross BBFRAME boundaries at the sender, so a lost frame cannot splice
    parts of two valid packets. CRC and SHA-256 are integrity checks, not auth.
    """
    offset = 0
    while True:
        offset = raw.find(MAGIC, offset)
        if offset < 0:
            return
        if offset + OVERHEAD <= len(raw):
            fields = HEADER.unpack_from(raw, offset)
            k, m, size = fields[-3:]
            end = offset + OVERHEAD + size
            if 1 <= k <= 255 and 1 <= m <= 255 and k + m <= 256 and size and end <= len(raw):
                packet = raw[offset:end]
                if zlib.crc32(packet[:-4]) == int.from_bytes(packet[-4:], "big"):
                    yield packet
                    offset = end
                    continue
        offset += 1


def recover_packets(packets):
    """Recover exactly one file from unordered packets; fail closed if incomplete.

    Ignore corrupt/truncated packets. Reject mixed transfers, contradictory valid
    duplicates, impossible sequence numbers, or a final SHA-256 mismatch.
    """
    metadata, blocks = None, {}
    for packet in packets:
        if len(packet) < OVERHEAD or zlib.crc32(packet[:-4]) != int.from_bytes(packet[-4:], "big"):
            continue
        magic, digest, length, sequence, k, m, size = HEADER.unpack_from(packet)
        if magic != MAGIC:
            raise ValueError("unsupported packet version")
        _params(k, m, size)
        if len(packet) != OVERHEAD + size:
            raise ValueError("packet length mismatch")
        current = digest, length, k, m, size
        if metadata is not None and metadata != current:
            raise ValueError("mixed files or coding parameters")
        metadata = current
        count = max(1, (length + k * size - 1) // (k * size))
        if count * (k + m) > 2**32 or sequence >= count * (k + m):
            raise ValueError("sequence outside file")
        block, index = divmod(sequence, k + m)
        received = blocks.setdefault(block, {})
        payload = packet[HEADER.size:-4]
        if index in received and received[index] != payload:
            raise ValueError("conflicting duplicate")
        received[index] = payload
    if metadata is None:
        raise ValueError("no valid packets")
    if len(blocks) != count:
        raise ValueError("missing blocks")
    output = bytearray()
    for block in range(count):
        received = blocks[block]
        if len(received) < k:
            raise ValueError(f"block {block}: need {k} distinct packets, have {len(received)}")
        indices = sorted(received)[:k]
        shards = [received[i] for i in indices]
        inverse = _inverse([_row(i, k, m) for i in indices])
        for row in inverse:
            output.extend(_combine(row, shards))
    result = bytes(output[:length])
    if hashlib.sha256(result).digest() != digest:
        raise ValueError("file SHA-256 mismatch")
    return result


def data_fields(packets, capacity):
    """Pack whole packets in each DATA FIELD, never straddling a BBFRAME."""
    field = bytearray()
    for packet in packets:
        if len(packet) > capacity:
            raise ValueError("packet exceeds BBFRAME DATA FIELD capacity")
        if len(field) + len(packet) > capacity:
            yield bytes(field)
            field.clear()
        field.extend(packet)
    if field:
        yield bytes(field)


def bbframes(data, rate="2/3", short=False, k=16, m=4, shard_size=None):
    """Yield LeanDVB records via the existing bit-exact DVB-S2 mode adapter.

    Default is one packet per frame, filling its DATA FIELD. The remaining
    Kbch-80-DFL bits, if any, are DVB-S2 zero padding, never application bytes.
    SYNC=0xb9 declares a user-private GCS protocol (EN 302 307-1 5.1.6).
    """
    import numpy as np
    import dvbs2

    capacity = (dvbs2.code(rate, short).kbch - 80) // 8
    size = capacity - OVERHEAD if shard_size is None else shard_size
    for field in data_fields(encode_packets(data, k, m, size), capacity):
        bits = np.unpackbits(np.frombuffer(field, dtype=np.uint8))
        frame = dvbs2.bbframe(bits, rate, short=short, sync=0xb9)
        yield np.packbits(frame[:80 + len(bits)]).tobytes()


def recover(raw, bbf=True):
    if bbf:
        from decode import parse_bbframes

        frames = parse_bbframes(raw)
        if any(f["stream_type"] != 1 for f in frames):
            raise ValueError("filelink requires GCS BBFRAMEs")
        # Scan per field as well as on flattened input; do not bridge erased frames.
        packets = (packet for f in frames for packet in packets_from_stream(f["payload"]))
    else:
        packets = packets_from_stream(raw)
    return recover_packets(packets)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    enc = commands.add_parser("encode", help="file -> LeanDVB-format .bbf records")
    enc.add_argument("source", type=Path)
    enc.add_argument("output", type=Path)
    enc.add_argument("--rate", default="2/3")
    enc.add_argument("--short", action="store_true")
    enc.add_argument("--data", type=int, default=16, dest="k")
    enc.add_argument("--repair", type=int, default=4, dest="m")
    enc.add_argument("--shard-size", type=int)
    dec = commands.add_parser("recover", help="LeanDVB .bbf records -> verified file")
    dec.add_argument("source", type=Path)
    dec.add_argument("output", type=Path)
    dec.add_argument("--payload", action="store_true", help="input is flattened GCS bytes")
    args = parser.parse_args()
    try:
        if args.command == "encode":
            result = b"".join(bbframes(args.source.read_bytes(), args.rate, args.short,
                                       args.k, args.m, args.shard_size))
        else:
            result = recover(args.source.read_bytes(), bbf=not args.payload)
        args.output.write_bytes(result)  # only publish after complete verification
    except (ValueError, KeyError, OSError) as exc:
        parser.exit(2, f"filelink: {exc}\n")


if __name__ == "__main__":
    main()
