"""File erasure coding over DVB-S2 GCS. See docs/file-link.md for the wire format.

Independent systematic Cauchy Reed-Solomon erasure code over GF(256), polynomial
0x11d. This is an application protocol, not RaptorQ, GSE or MPE.
One packet fills one DATA FIELD. Encoding spools input and buffers one block;
recovery buffers the received transfer. This module needs only the stdlib.
"""
from functools import lru_cache
import hashlib
import io
import struct
import tempfile
import zlib


MAGIC = b"PFL1"
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


def parameters(value):
    """Parse the senders' --fec K:M option, before opening any hardware."""
    try:
        k, m = map(int, value.split(':'))
        _params(k, m, 1)
        return k, m
    except (ValueError, TypeError):
        raise ValueError('FEC must be K:M with K,M >= 1 and K+M <= 256') from None


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
    yield from encode_stream(io.BytesIO(data), shard_size + OVERHEAD, k, m)


def encode_stream(source, capacity, k=16, m=4):
    """Yield exactly one capacity-byte packet per frame, snapshotting short reads."""
    size = capacity - OVERHEAD
    _params(k, m, size)
    with tempfile.TemporaryFile() as snapshot:
        digest, length = hashlib.sha256(), 0
        while part := source.read(65536):
            snapshot.write(part)
            digest.update(part)
            length += len(part)
        snapshot.seek(0)
        yield from _encode(snapshot, digest.digest(), length, k, m, size)


def _encode(source, digest, length, k, m, shard_size):
    blocks = max(1, (length + k * shard_size - 1) // (k * shard_size))
    if blocks * (k + m) > 2**32:
        raise ValueError("file exceeds sequence space")
    for block in range(blocks):
        padded = source.read(k * shard_size).ljust(k * shard_size, b"\0")
        shards = [padded[i * shard_size:(i + 1) * shard_size] for i in range(k)]
        for index in range(k + m):
            payload = shards[index] if index < k else _combine(_row(index, k, m), shards)
            packet = HEADER.pack(MAGIC, digest, length, block * (k + m) + index,
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


def write_stream(source, target, capacity, k=16, m=4):
    """Write acquisition frames, FEC frames, then drain frames; all frame aligned."""
    _params(k, m, capacity - OVERHEAD)
    target.write(bytes(8 * capacity))
    for packet in encode_stream(source, capacity, k, m):
        target.write(packet)
    target.write(bytes(4 * capacity))
    target.seek(0)


def recover(raw, bbf=False):
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
