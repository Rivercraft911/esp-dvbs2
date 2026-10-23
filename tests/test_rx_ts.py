"""Compile the receiver's TS deframer (firmware/components/dvbs2rx/rx_ts.c) and check it against
host/dvbs2.py ts_bbframes: every packet back, bit exact, through straddling packets, packets that
end with a frame, gaps, late frames, corrupted CRC-8s and bad SYNCDs; continuity counters by the
ISO/IEC 13818-1 rules. Run: python3 tests/test_rx_ts.py. No ESP-IDF, board or pytest needed.
"""
import ctypes as ct
import os
import pathlib
import shlex
import subprocess
import sys
import tempfile

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "host"))
import dvbs2 as d  # noqa: E402

COMPONENT = ROOT / "firmware/components/dvbs2rx"
EMIT = ct.CFUNCTYPE(None, ct.c_void_p, ct.POINTER(ct.c_uint8))
FIELDS = ("frames", "packets", "crc_errors", "unchecked", "null_packets", "cc_errors", "resyncs",
          "syncd_errors", "not_ts", "late", "dropped_bytes", "untracked")


class Stats(ct.Structure):
    _fields_ = [(name, ct.c_uint32) for name in FIELDS]


class Pid(ct.Structure):
    _fields_ = [("pid", ct.c_uint16), ("packets", ct.c_uint32), ("cc_errors", ct.c_uint32)]


class Frame(ct.Structure):
    _fields_ = [("llr", ct.POINTER(ct.c_int8)), ("hard", ct.POINTER(ct.c_uint8)),
                ("weak", ct.POINTER(ct.c_uint8)), ("header_sample", ct.c_uint64), ("sequence", ct.c_uint32),
                ("cfo_hz", ct.c_int32), ("clock_ppb", ct.c_int32), ("mer_cdb", ct.c_int16),
                ("modcod", ct.c_uint8), ("type", ct.c_uint8)]


def build(folder):
    """The deframer, with the FEC check (and the transmitter's encoder it uses) for the link test."""
    lib, encoder = pathlib.Path(folder) / "rx_ts.so", ROOT / "firmware/components/dvbs2"
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-fPIC", "-shared",
        "-I", str(COMPONENT / "include"), "-I", str(COMPONENT), "-I", str(encoder / "include"),
        str(COMPONENT / "rx_ts.c"), str(COMPONENT / "rx_fec.c"), str(encoder / "dvbs2.c"),
        str(encoder / "fast.c"), "-o", str(lib)], check=True)
    so = ct.CDLL(str(lib))
    so.rx_fec_check_size.restype = ct.c_size_t
    so.rx_fec_check_size.argtypes = [ct.c_uint]
    so.rx_fec_init.argtypes = [ct.c_void_p, ct.c_uint, ct.c_void_p]
    so.rx_fec_frame.argtypes = [ct.c_void_p, ct.POINTER(Frame), ct.POINTER(ct.POINTER(ct.c_uint8)),
                                ct.c_void_p]
    so.rx_ts_size.restype = ct.c_size_t
    so.rx_ts_init.argtypes = [ct.c_void_p, ct.c_int, EMIT, ct.c_void_p]
    so.rx_ts_frame.restype = ct.c_uint
    so.rx_ts_frame.argtypes = [ct.c_void_p, ct.c_void_p, ct.c_uint, ct.c_uint32]
    so.rx_ts_flush.argtypes = [ct.c_void_p]
    so.rx_ts_stats.argtypes = [ct.c_void_p, ct.POINTER(Stats)]
    so.rx_ts_pid.argtypes = [ct.c_void_p, ct.c_uint, ct.POINTER(Pid)]
    return so


class Deframer:
    def __init__(self, so, drop_null=False):
        self.so, self.out = so, []
        self.mem = ct.create_string_buffer(so.rx_ts_size())
        self.emit = EMIT(lambda ctx, p: self.out.append(bytes(p[:188])))
        so.rx_ts_init(self.mem, int(drop_null), self.emit, None)

    def frame(self, bbframe, sequence):
        buf = ct.create_string_buffer(bytes(bbframe), len(bbframe))
        return self.so.rx_ts_frame(self.mem, buf, len(bbframe) - 10, sequence)

    def run(self, frames, sequences=None):
        for k, f in enumerate(frames):
            self.frame(f, k if sequences is None else sequences[k])
        return self

    def flush(self):
        self.so.rx_ts_flush(self.mem)
        return self

    def stats(self):
        s = Stats()
        self.so.rx_ts_stats(self.mem, ct.byref(s))
        return {name: getattr(s, name) for name in FIELDS}

    def pids(self):
        out, p, i = {}, Pid(), 0
        while self.so.rx_ts_pid(self.mem, i, ct.byref(p)):
            out[p.pid] = (p.packets, p.cc_errors)
            i += 1
        return out


def ts_stream(n, rng, pids=(0x000, 0x100, 0x101), nulls=0.1):
    """n TS packets: payload-only packets on a few PIDs with continuous counters, some with an
    adaptation field, and null packets."""
    cc, out = {p: int(rng.integers(16)) for p in pids}, []
    for _ in range(n):
        pid = 0x1fff if rng.random() < nulls else int(rng.choice(pids))
        p = bytearray(rng.integers(0, 256, 188, dtype=np.uint8).tobytes())
        p[0] = 0x47
        p[1] = (int(rng.random() < 0.2) << 6) | pid >> 8
        p[2] = pid & 255
        if pid == 0x1fff:
            p[3] = 0x10
        else:
            cc[pid] = (cc[pid] + 1) & 15
            adaptation = rng.random() < 0.2
            p[3] = (0x30 if adaptation else 0x10) | cc[pid]
            if adaptation:
                p[4], p[5] = 7, 0x10                    # length 7, PCR flag, no discontinuity
        out.append(bytes(p))
    return out


def frames_for(packets, dfl_bytes, sync=0x47, first_crc=0):
    """BBFRAMEs (header and data field, unscrambled) of any byte DFL, as ts_bbframes cuts them:
    SYNCD to the next packet start, 65,535 when none starts in the field."""
    pk = np.frombuffer(b"".join(packets), np.uint8).reshape(-1, 188).copy()
    pk[:, 0] = [first_crc] + [d.crc8(np.unpackbits(p[1:])) for p in pk[:-1]]
    stream, upl, dfl = pk.ravel(), 1504, 8 * dfl_bytes
    out = []
    for k in range(len(stream) // dfl_bytes):
        syncd = (-k * dfl) % upl
        h = d.bbheader(d.matype1(d.TS), dfl, upl, sync, syncd if syncd < dfl else 0xffff)
        out.append(np.packbits(h).tobytes() + stream[k * dfl_bytes:(k + 1) * dfl_bytes].tobytes())
    return out


def expected(packets, dfl_bytes, received):
    """Packets a correct deframer hands on, given which frames arrive (in order): each packet
    whose bytes all arrive in a run of consecutive frames that starts at a packet start the
    deframer can see, with its CRC-8 (the next packet's first byte) either there or lost to a
    gap or the end (then unchecked)."""
    out, unchecked, run = [], 0, []
    received = list(received) + [None]
    for k in received:
        if run and (k is None or k != run[-1] + 1):
            lo, hi = run[0] * dfl_bytes, (run[-1] + 1) * dfl_bytes
            first = -(-lo // 188)
            for i in range(first, len(packets)):
                if 188 * (i + 1) > hi:
                    break
                out.append(packets[i])
                unchecked += 188 * (i + 1) == hi
            run = []
        if k is not None:
            run.append(k)
    return out, unchecked


def cc_errors(packets):
    """Continuity errors of a packet list by the ISO/IEC 13818-1 rules (no duplicates here)."""
    last, errors = {}, 0
    for p in packets:
        pid = (p[1] & 0x1f) << 8 | p[2]
        if pid == 0x1fff:
            continue
        cc = p[3] & 15
        if pid in last and cc != (last[pid] + (p[3] >> 4 & 1)) & 15:
            errors += 1
        last[pid] = cc
    return errors


def test_exact(so):
    """Standard data fields (Kbch - 80) of short and normal frames: the deframer gives back every
    packet that ts_bbframes put in, bit exact."""
    rng = np.random.default_rng(1)
    packets = ts_stream(400, rng)
    for rate, short in (("1/4", True), ("3/4", True), ("8/9", True), ("3/4", False)):
        dfl = (d.code(rate, short).kbch - 80) // 8
        reference = [np.packbits(f).tobytes() for f in d.ts_bbframes(np.frombuffer(b"".join(packets), np.uint8), rate, short)]
        frames = frames_for(packets, dfl)
        assert frames == reference[:len(frames)] and len(frames) == len(reference), (rate, short)
        r = Deframer(so).run(frames)
        want, _ = expected(packets, dfl, range(len(frames)))
        s = r.stats()
        assert r.out == want[:len(r.out)] and len(r.out) == len(want) - (len(frames) * dfl % 188 == 0)
        assert s["crc_errors"] == s["cc_errors"] == s["resyncs"] == s["dropped_bytes"] == 0, s
        assert s["packets"] == len(r.out) and s["frames"] == len(frames), s
        assert s["null_packets"] == sum(p[1] & 0x1f == 0x1f and p[2] == 0xff for p in r.out), s
        assert r.flush().out == want, (rate, short)
    print("exact: QPSK 1/4, 3/4, 8/9 short and 3/4 normal data fields, every packet back: ok")


def test_boundaries(so):
    """Fields of whole packets (a packet ends with every frame: its CRC-8 opens the next one), a
    field shorter than a packet (SYNCD 65,535), and a stream picked up mid-way."""
    rng = np.random.default_rng(2)
    packets = ts_stream(120, rng)
    for dfl in (188 * 5, 188, 100, 187, 189, 1475):
        frames = frames_for(packets, dfl)
        r = Deframer(so).run(frames).flush()
        want, unchecked = expected(packets, dfl, range(len(frames)))
        s = r.stats()
        assert r.out == want and s["unchecked"] == unchecked and s["crc_errors"] == 0, (dfl, s)
        if dfl == 100:
            assert any(f[7:9] == b"\xff\xff" for f in frames)
        for start in (1, 3, 7):
            r = Deframer(so).run(frames[start:], range(start, len(frames)))
            dropped = -(-start * dfl // 188) * 188 - start * dfl
            assert r.stats()["dropped_bytes"] == dropped, (dfl, start, r.stats())
            want, _ = expected(packets, dfl, range(start, len(frames)))
            assert r.flush().out == want, (dfl, start)
    print("boundaries: whole-packet fields, SYNCD 65,535, fields of 187 to 1,475 bytes, "
          "starting mid-stream: ok")


def test_gaps(so):
    """Lost frames: the deframer restarts at the next SYNCD, a packet complete before the gap
    goes on unchecked, partial ones are dropped; continuity errors count the lost packets. A late
    frame (the decoder's) is ignored."""
    rng = np.random.default_rng(3)
    packets = ts_stream(300, rng, nulls=0)
    for dfl in (1475, 188 * 4, 1769):
        frames = frames_for(packets, dfl)
        for lost in ([5], [5, 6], [2, 9, 10, 17]):
            keep = [k for k in range(len(frames)) if k not in lost]
            r = Deframer(so).run([frames[k] for k in keep], keep).flush()
            want, unchecked = expected(packets, dfl, keep)
            s = r.stats()
            assert r.out == want, (dfl, lost)
            assert s["unchecked"] == unchecked and s["resyncs"] == len([k for k in lost if k - 1 not in lost])
            assert s["cc_errors"] == cc_errors(want) > 0 and s["crc_errors"] == 0, (dfl, lost, s)
        r = Deframer(so)
        for k in range(10):
            r.frame(frames[k], k)
            if k == 4:
                r.frame(frames[3], 3)
        assert r.stats()["late"] == 1 and r.stats()["resyncs"] == 0
        assert r.flush().out == expected(packets, dfl, range(10))[0]
    print("gaps: one, two and scattered lost frames, unchecked and dropped packets, continuity "
          "errors, a late frame: ok")


def test_crc(so):
    """A flipped bit inside a packet, in a CRC-8, or in a packet that straddles two frames: only
    that packet is marked (transport_error_indicator), with its received bytes."""
    rng = np.random.default_rng(4)
    packets = ts_stream(200, rng, nulls=0)
    dfl = 1475
    clean = frames_for(packets, dfl)
    want, _ = expected(packets, dfl, range(len(clean)))
    # Stream byte positions: inside packet 20; packet 31's CRC-8 (packet 32's first byte); packet
    # 39, which frames 4 and 5 share (frame 4 ends at stream byte 7,375), in either part.
    across = 7375 // 188
    for at, victim in ((20 * 188 + 77, 20), (32 * 188, 31), (across * 188 + 10, across),
                       (7375 + 2, across)):
        frames = [bytearray(f) for f in clean]
        frames[at // dfl][10 + at % dfl] ^= 0x10
        r = Deframer(so).run(frames).flush()
        s = r.stats()
        assert s["crc_errors"] == 1 and len(r.out) == len(want), (at, s)
        bad = [i for i, (a, b) in enumerate(zip(r.out, want)) if a != b]
        assert bad == [victim], (at, bad)
        p = bytearray(want[victim])
        p[1] |= 0x80
        if at % 188:
            p[at % 188] ^= 0x10
        assert r.out[victim] == bytes(p) and s["cc_errors"] == 0, at
    print("crc-8: errors inside, in the CRC byte and across frames mark exactly one packet: ok")


def test_headers(so):
    """A frame lost without a sequence gap (its SYNCD then disagrees with the carried packet) and
    a frame that is not TS restart the deframer."""
    rng = np.random.default_rng(5)
    packets = ts_stream(200, rng, nulls=0)
    dfl = 1475
    frames = frames_for(packets, dfl)
    keep = [k for k in range(len(frames)) if k != 6]
    r = Deframer(so).run([frames[k] for k in keep]).flush()
    s = r.stats()
    assert s["syncd_errors"] == 1 and s["resyncs"] == 1 and s["crc_errors"] == 0, s
    assert r.out == expected(packets, dfl, keep)[0]
    gcs = bytearray(frames[8])
    gcs[:10] = np.packbits(d.bbheader(d.matype1(d.GCS), 8 * dfl)).tobytes()
    r = Deframer(so).run(frames[:8] + [gcs] + frames[9:]).flush()
    s = r.stats()
    assert s["not_ts"] == 1 and s["resyncs"] == 1 and r.out == expected(
        packets, dfl, [k for k in range(len(frames)) if k != 8])[0], s
    print("headers: an unseen lost frame (SYNCD disagrees) and a GCS frame restart the deframer: ok")


def test_continuity(so):
    """ISO/IEC 13818-1 2.4.3.3: one duplicate (same bytes, a new PCR allowed), adaptation-only
    packets, the discontinuity indicator; repeats with other bytes, a third copy and a payload
    packet reusing an adaptation-only packet's counter are errors. Null packets dropped on
    request, and PIDs past the tracked ones."""
    def packet(pid, cc, control=1, discontinuity=False, pcr=None, fill=0):
        p = bytearray([fill]) * 188
        p[0], p[1], p[2], p[3] = 0x47, pid >> 8, pid & 255, control << 4 | cc
        if control & 2:
            p[4], p[5] = 1, 0x80 if discontinuity else 0
        if pcr is not None:
            p[4], p[5], p[6:12] = 7, 0x10, pcr.to_bytes(6, "big")
        return bytes(p)
    seq = [packet(0x100, 0), packet(0x100, 1, fill=1),
           packet(0x100, 1, fill=1),                                  # a duplicate: fine
           packet(0x100, 1, fill=1),                                  # a third copy: error
           packet(0x100, 2, fill=2), packet(0x100, 2, fill=3),        # other bytes: error
           packet(0x100, 3), packet(0x100, 3, control=2),             # no payload: stays
           packet(0x100, 3),                                          # payload, same counter: error
           packet(0x100, 4, control=3, pcr=1000, fill=4),
           packet(0x100, 4, control=3, pcr=1300, fill=4),             # duplicate, new PCR: fine
           packet(0x100, 5, control=3, pcr=1600, fill=5),
           packet(0x100, 5, control=3, pcr=1600, fill=6),             # PCR the same, bytes not: error
           packet(0x100, 8),                                          # jump: error
           packet(0x100, 12, control=3, discontinuity=True),          # flagged jump: fine
           packet(0x100, 13), packet(0x1fff, 0), packet(0x1fff, 7),
           packet(0x101, 3), packet(0x101, 3, control=2), packet(0x101, 4), packet(0x101, 5),
           packet(0x101, 6)]
    seq += [packet(0x200 + k, 0) for k in range(20)] + [packet(0x100, 14)]   # 3 per field
    frames = frames_for(seq + [packet(0x100, 15)], 188 * 3)
    for drop in (False, True):
        r = Deframer(so, drop_null=drop).run(frames)
        s = r.stats()
        assert s["cc_errors"] == 5 and s["null_packets"] == 2 and s["untracked"] == 6, s
        assert r.out == [p for p in seq if not (drop and p[1] & 0x1f == 0x1f)], drop
        assert r.pids()[0x100] == (17, 5) and r.pids()[0x101] == (5, 0) and len(r.pids()) == 16
    print("continuity: duplicates checked by content (PCR aside), no payload, discontinuity "
          "indicator, nulls, 16 PIDs: ok")


def test_link(so, folder):
    """The whole receiver on the host: the transmitter's TS replay input (host/rx.py ts_model) in
    8PSK 8/9 short frames at 5 Msym/s with dummy PLFRAMEs between them, through the twin channel,
    the C demodulator, the FEC check and the deframer, gives back the replay's packets."""
    import rx
    import rx_demod as R
    import rx_twin as T
    modcod = 16
    dfl = (d.code("8/9", True).kbch - 80) // 8
    stream = b"".join(rx.ts_messages(0, 30, True))
    packets = [stream[i:i + 188] for i in range(0, len(stream), 188)]
    link = T.Link(rate=5e6, modcod=modcod, frames=14, mer_db=24, seed=10, ts=True,
                  dummies=(3, 6, 6, 9) + (11,) * 12)
    words, truth = T.twin(link)
    demod = R.Demod(R.build(folder), modcod, rate=5e6, cfo_hint=-16600, cfo_span=40000)
    demod.feed(words)
    check = ct.create_string_buffer(so.rx_fec_check_size(modcod) + 16)
    fec = ct.addressof(check) + (-ct.addressof(check) % 16)
    assert so.rx_fec_init(fec, modcod, None) == 0
    r = Deframer(so)
    received = []
    for f in demod.frames:
        hard = (ct.c_uint8 * 2048)()
        ct.memmove(hard, np.packbits(f["hard"]).tobytes(), 2025)
        frame = Frame(None, hard, None, 0, f["sequence"], 0, 0, 0, f["modcod"], f["type"])
        payload = ct.POINTER(ct.c_uint8)()
        n = so.rx_fec_frame(fec, ct.byref(frame), ct.byref(payload), None)
        assert n == dfl, n
        r.so.rx_ts_frame(r.mem, hard, n, f["sequence"])
        received.append(min(truth, key=lambda t: abs(t["header_sample"] - f["header_sample"]))["model_frame"])
    r.flush()
    s = r.stats()
    assert received == list(range(1, 14)) and demod.stats()["dummies"] == 16, (received, demod.stats())
    assert r.out == expected(packets, dfl, received)[0] and s["crc_errors"] == s["resyncs"] == 0, s
    assert s["cc_errors"] == sum(a[6] == 6 and a[4] == 7 for a in r.out[:-1]), s   # replay wraps
    print(f"link: twin 8PSK 8/9 TS frames with 16 dummies, demodulated, checked and deframed: "
          f"{s['packets']} packets exact: ok")


def test_pattern(folder):
    """The receiver checks `rx ... ts pattern` packets with ingress_pattern_ts_differs: zero for the
    transmitter's own packets (host/spi_input.py pattern), nonzero for any changed byte."""
    from spi_input import pattern
    ingress = ROOT / "firmware/components/ingress"
    lib = pathlib.Path(folder) / "pattern.so"
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-fPIC", "-shared", "-I", str(ingress / "include"),
        str(ingress / "ingress_pattern.c"), "-o", str(lib)], check=True)
    so = ct.CDLL(str(lib))
    so.ingress_pattern_ts_differs.restype = ct.c_uint32
    so.ingress_pattern_ts_differs.argtypes = [ct.c_char_p, ct.c_uint16, ct.c_uint]
    so.ingress_pattern_packet.argtypes = [ct.c_void_p, ct.c_uint16, ct.c_uint, ct.c_uint]
    rng = np.random.default_rng(6)
    for seq in (0, 1, 7, 8, 255, 256, 4097, 65535):
        message = pattern(seq, 1)
        for j in range(7):
            p = message[188 * j:188 * (j + 1)]
            out = ct.create_string_buffer(188)
            so.ingress_pattern_packet(out, seq, j, 1)
            assert out.raw == p and so.ingress_pattern_ts_differs(p, seq, j) == 0, (seq, j)
            for k in [0, 3, 4, 6, 7, 8, 187] + list(rng.integers(0, 188, 4)):
                bad = bytearray(p)
                bad[k] ^= 1 << int(rng.integers(8))
                assert so.ingress_pattern_ts_differs(bytes(bad), seq, j), (seq, j, k)
        assert so.ingress_pattern_ts_differs(message[:188], seq, 7)
    print("pattern: the transmitter's packets pass, any flipped bit fails: ok")


def main():
    with tempfile.TemporaryDirectory() as tmp:
        test_pattern(tmp)
        so = build(tmp)
        for test in (test_exact, test_boundaries, test_gaps, test_crc, test_headers, test_continuity):
            test(so)
        test_link(so, tmp)
    print("rx ts deframer passed")


if __name__ == "__main__":
    main()
