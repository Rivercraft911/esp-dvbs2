"""Packet erasures, GCS mapping, CLI failures, optionally independent LeanDVB IQ.

python3 tests/test_filelink.py
python3 tests/test_filelink.py --loopback --receiver /path/to/leandvb [--out runs.jsonl]
"""
import argparse
from datetime import datetime, timezone
import hashlib
import itertools
import json
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile
import zlib

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "host"))
import decode as rx
import dvbs2 as dvb
import filelink as link


def rejected(function, *args, **kwargs):
    try:
        function(*args, **kwargs)
    except ValueError:
        return
    raise AssertionError("invalid or incomplete transfer accepted")


def reseal(packet):
    return packet[:-4] + struct.pack(">I", zlib.crc32(packet[:-4]))


def test_field():
    # Independent polynomial long multiplication, no exp/log tables.
    def slow(a, b):
        result = 0
        while b:
            if b & 1:
                result ^= a
            b >>= 1
            a <<= 1
            if a & 256:
                a ^= 0x11d
        return result

    assert link.OVERHEAD == 56
    assert len(set(link.EXP[:255])) == 255
    for a in range(256):
        for b in range(256):
            assert link.mul(a, b) == slow(a, b)
        if a:
            assert link.mul(a, link.inv(a)) == 1
    # Hand-computed GF(256) Cauchy vector: k=2,m=2, x={2,3},y={0,1}.
    # inv(2)=142, inv(3)=244. [1,2] -> parity [123,245].
    packets = list(link.encode_packets(b"\1\2", 2, 2, 1))
    assert [p[link.HEADER.size:-4] for p in packets] == [b"\1", b"\2", b"\x7b", b"\xf5"]
    for k, m in ((1, 3), (2, 2), (3, 3), (4, 3)):
        original = bytes(range(k * 7 - 2))
        packets = list(link.encode_packets(original, k, m, 7))
        for indices in itertools.combinations(range(k + m), k):
            assert link.recover_packets([packets[i] for i in indices]) == original
    for k, m in ((255, 1), (1, 255), (128, 128)):
        original = bytes(i % 256 for i in range(k))
        packets = list(link.encode_packets(original, k, m, 1))
        assert link.recover_packets(packets[-k:]) == original
    for params in ((0, 4, 10), (16, 0, 10), (255, 2, 1), (1, 1, 0), (1, 1, 65536)):
        rejected(lambda: list(link.encode_packets(b"x", *params)))


def test_packets():
    rng = random.Random(20261016)
    for length in (0, 1, 31, 32, 33, 199, 200, 201, 3011):
        original = rng.randbytes(length)
        packets = list(link.encode_packets(original, 4, 3, 25))
        received = []
        for base in range(0, len(packets), 7):
            received.extend(rng.sample(packets[base:base + 7], 4))
        received += received[:3]
        rng.shuffle(received)
        assert link.recover_packets(received) == original
    original = rng.randbytes(133)
    packets = list(link.encode_packets(original, 4, 3, 40))
    assert link.recover_packets(packets[3:]) == original
    rejected(link.recover_packets, packets[4:])
    rejected(link.recover_packets, [packets[0]] * 10)
    rejected(link.recover_packets, [])
    other = list(link.encode_packets(b"different file", 4, 3, 40))
    rejected(link.recover_packets, packets + other)
    # Corrupt an entire source packet, remove two others: exactly m erasures.
    damaged = bytearray(packets[0])
    damaged[-7] ^= 128
    assert link.recover_packets([bytes(damaged), *packets[3:]]) == original
    assert link.recover_packets([packets[0][:-1], *packets[3:]]) == original
    # A valid packet CRC cannot hide a corrupted file from the whole-file digest.
    wrong = reseal(bytes(damaged))
    rejected(link.recover_packets, [wrong, *packets[1:4]])
    rejected(link.recover_packets, [wrong, *packets])
    # Metadata and length attacks, with a recomputed CRC, must fail cleanly.
    fields = list(link.HEADER.unpack(packets[0][:link.HEADER.size]))
    fields[3] = 100000
    invalid = reseal(link.HEADER.pack(*fields) + packets[0][link.HEADER.size:])
    rejected(link.recover_packets, [invalid, *packets])
    rejected(link.recover_packets, [reseal(packets[0][:-5] + b"\0" * 4), *packets])
    raw = b"garbageSFL1" + bytes(damaged) + b"\0" + b"".join(packets[3:]) + b"SFL1"
    assert link.recover(raw, bbf=False) == original
    # Whole missing block and truncation cannot silently produce a shorter file.
    multi = list(link.encode_packets(b"a" * 400, 4, 3, 40))
    rejected(link.recover_packets, multi[7:])
    rejected(link.recover, b"".join(packets[:3])[:-1], bbf=False)


def record(field):
    return np.packbits(dvb.bbheader(dvb.matype1(dvb.GCS), len(field) * 8,
                                    sync=0xb9)).tobytes() + field


def test_frames():
    rng = random.Random(71)
    original = rng.randbytes(12000)
    # One packet per frame: erase any four of twenty in each codeword.
    for rate, short in (("1/4", True), ("1/2", False), ("2/3", False), ("9/10", False)):
        frames = list(link.bbframes(original, rate, short))
        decoded = rx.parse_bbframes(b"".join(frames))
        capacity = (dvb.code(rate, short).kbch - 80) // 8
        for raw, frame in zip(frames, decoded):
            assert frame["dfl_bits"] == capacity * 8
            bits = np.unpackbits(np.frombuffer(frame["payload"], np.uint8))
            expected = dvb.bbframe(bits, rate, short, sync=0xb9)
            assert raw == np.packbits(expected).tobytes()
        kept = []
        for base in range(0, len(frames), 20):
            group = frames[base:base + 20]
            kept.extend(group[4:])
        rng.shuffle(kept)
        assert link.recover(b"".join(kept)) == original
        payload = b"".join(f[10:] for f in kept)
        assert link.recover(payload, bbf=False) == original
        # More than m losses in the first block, even if every later block is intact.
        rejected(link.recover, b"".join(frames[5:]))
    # Multiple packets per frame: four lost packets from one dropped field.
    packets = list(link.encode_packets(original[:8000], 16, 4, 1024))
    fields = list(link.data_fields(packets, 5370))
    assert len(fields) == 5 and len(fields[0]) == 4320
    frames = [record(field) for field in fields]
    assert link.recover(b"".join(frames[1:])) == original[:8000]
    rejected(link.recover, b"".join(frames[2:]))
    # Non-full field uses DFL; BB padding never enters the application scanner.
    frame = next(link.bbframes(b"tiny", shard_size=123, k=2, m=1))
    field = rx.parse_bbframes(frame)[0]["payload"]
    assert len(field) == 3 * (123 + 56) < 5370
    rejected(lambda: list(link.data_fields(packets, 100)))
    ts = np.packbits(dvb.bbheader(dvb.matype1(dvb.TS), 0, upl=1504, sync=0x47)).tobytes()
    rejected(link.recover, ts)
    rejected(link.recover, frame[:-1])


def test_cli():
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory)
        original = bytes(range(256)) * 30
        (path / "source").write_bytes(original)
        command = [sys.executable, str(ROOT / "host/filelink.py")]
        subprocess.run(command + ["encode", str(path / "source"), str(path / "file.bbf")], check=True)
        subprocess.run(command + ["recover", str(path / "file.bbf"), str(path / "out")], check=True)
        assert (path / "out").read_bytes() == original
        (path / "file.bbf").write_bytes(b"bad")
        result = subprocess.run(command + ["recover", str(path / "file.bbf"), str(path / "out")],
                                capture_output=True)
        assert result.returncode == 2 and (path / "out").read_bytes() == original


def loopback(receiver):
    """Real separate decoder process on synthetic noiseless IQ, then erase records.

    Erasures are injected AFTER FEC decoding. This proves the output interface and
    file recovery, not demodulator reacquisition through an RF outage.
    """
    import iq

    original = random.Random(20261016).randbytes(11000)
    records = list(link.bbframes(original, k=3, m=2))
    symbols = []
    for raw in records:
        bits = np.unpackbits(np.frombuffer(raw[10:], np.uint8))
        frame = dvb.bbframe(bits, "2/3", sync=0xb9)
        symbols.append(dvb.plframe_symbols(dvb.fecframe(frame, "2/3"), "2/3",
                                           modulation="8psk", pilots=True))
    guard = dvb.transmit_symbols(np.zeros(100, np.uint8), "2/3", modulation="8psk", pilots=True)
    symbols = np.concatenate([np.tile(guard, 4), *symbols, np.tile(guard, 8)])
    samples = iq.shape_symbols(symbols, 5e6, 10e6, alpha=0.35)
    with tempfile.TemporaryFile() as source, tempfile.TemporaryFile() as output, \
            tempfile.TemporaryFile() as diagnostics:
        source.write(np.asarray(samples, dtype="<c8").tobytes())
        source.seek(0)
        command = [str(receiver), "--standard", "DVB-S2", "--f32", "-f", "10000000",
                   "--sr", "5000000", "--sampler", "rrc", "--roll-off", "0.35",
                   "--ldpc-bf", "100", "--fd-bbf", str(output.fileno()), "--fd-info", "2", "-d"]
        subprocess.run(command, stdin=source, stdout=subprocess.DEVNULL, stderr=diagnostics,
                       pass_fds=(output.fileno(),), timeout=120, check=True)
        output.seek(0)
        parsed = rx.parse_bbframes(output.read())
        diagnostics.seek(0)
        status = diagnostics.read().decode(errors="replace")
    received = [record(f["payload"]) for f in parsed if f["payload"].startswith(link.MAGIC)]
    assert received == records
    report = rx.summarize(status, parsed, b"")
    assert report["frame_lock"] and report["bch_failed_frames"] == 0
    assert report["ldpc_checks_after"] == 0
    # Exhaust every pair of lost BBFRAMEs (including both first and both last).
    pairs = list(itertools.combinations(range(5), 2))
    for erased in pairs:
        kept = [frame for i, frame in enumerate(received) if i not in erased]
        assert link.recover(b"".join(kept)) == original
        assert link.recover(b"".join(frame[10:] for frame in kept), bbf=False) == original
    rejected(link.recover, b"".join(received[3:]))
    return {"evidence": "simulated IQ; real LeanDVB process; post-decoder erasures",
            "time_utc": datetime.now(timezone.utc).isoformat(),
            "source_commit": subprocess.check_output(
                ["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True).strip(),
            "source_dirty": bool(subprocess.check_output(
                ["git", "-C", str(ROOT), "status", "--porcelain"], text=True).strip()),
            "source_sha256": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                              for p in (ROOT / "host/filelink.py", ROOT / "tests/test_filelink.py",
                                        ROOT / "host/dvbs2.py", ROOT / "host/iq.py",
                                        ROOT / "host/decode.py")},
            "python": sys.version.split()[0], "numpy": np.__version__, "firmware": None,
            "file_bytes": len(original), "k": 3, "m": 2, "file_bbframes": len(received),
            "symbol_rate": 5000000, "sample_rate": 10000000, "modcod": 13,
            "two_frame_loss_patterns": len(pairs), "sha256": hashlib.sha256(original).hexdigest(),
            "sha256_match": True, "three_frame_loss_rejected": True,
            "receiver_sha256": hashlib.sha256(Path(receiver).read_bytes()).hexdigest(),
            "bch_failed_frames": report["bch_failed_frames"],
            "ldpc_checks_after": report["ldpc_checks_after"]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--loopback", action="store_true")
    parser.add_argument("--receiver", type=Path, default=rx.DEFAULT_RECEIVER)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    test_field()
    test_packets()
    test_frames()
    test_cli()
    print("filelink: GF(256), any-k recovery, CRC/SHA failures, lost BBFRAMEs and CLI ok", flush=True)
    if args.loopback:
        result = loopback(args.receiver)
        print(json.dumps(result, sort_keys=True), flush=True)
        if args.out:
            with args.out.open("a") as output:
                output.write(json.dumps(result, sort_keys=True) + "\n")
