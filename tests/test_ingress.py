"""Check the PV message format and the ingress slot ring on the host, or board spirx records.

    python3 tests/test_ingress.py                                   # native checks
    python3 tests/test_ingress.py results/<date>-spirx/runs.jsonl   # pass criteria per record

The native part compiles firmware/components/ingress/ingress.c with tests/ingress_sim.c, a model
of the ESP32-S3 GDMA receive channel, and drives it with ctypes. No ESP-IDF, board or pytest.
"""
import ctypes as ct
import json
import os
import pathlib
import random
import shlex
import subprocess
import sys
import tempfile
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
MSG, HEADER, PAYLOAD, PACKET = 1332, 12, 1316, 188
SLOTS, SLOT_BYTES, MARGIN = 4, 1336, 2
NOP, TS, GCS = 0, 1, 2
FAULT_EVERY = 64
STATS = ("msgs", "bytes", "nops", "err_short", "err_long", "err_header", "err_crc",
         "err_sync", "lost", "seq_back", "overruns", "restarts", "ready_falls", "ts_packets", "crc_chain")


class Stats(ct.Structure):
    _fields_ = [(name, ct.c_uint32) for name in STATS]


def pv_message(seq, payload, kind=TS):
    """Independent PV-SPI v1 serialization."""
    head = b"PV" + bytes([1, kind]) + (seq & 0xFFFF).to_bytes(2, "little")
    head += len(payload).to_bytes(2, "little") + bytes(4)
    body = head + payload + bytes(PAYLOAD - len(payload))
    return body + zlib.crc32(body).to_bytes(4, "little")


def ts_payload(rng, packets=7):
    out = bytearray()
    for _ in range(packets):
        out += b"\x47" + bytes(rng.getrandbits(8) for _ in range(PACKET - 1))
    return bytes(out)


class Native:
    def __enter__(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="ingress-")
        library = pathlib.Path(self.tmp.name) / "ingress.so"
        component = ROOT / "firmware/components/ingress"
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-fPIC", "-shared",
            "-I", str(component / "include"), str(component / "ingress.c"), str(component / "ingress_pattern.c"),
            str(ROOT / "tests/ingress_sim.c"), "-o", str(library)], check=True)
        lib = self.lib = ct.CDLL(str(library))
        api = {
            "ingress_pv_pattern": (None, [ct.c_void_p, ct.c_uint16]),
            "ingress_pattern": (None, [ct.c_void_p, ct.c_uint16, ct.c_uint]),
            "sim_type": (None, [ct.c_void_p, ct.c_uint]),
            "sim_wrong_type": (ct.c_uint, [ct.c_void_p]),
            "pv_crc32_tables": (None, [ct.POINTER(ct.c_uint32)]),
            "pv_crc32": (ct.c_uint32, [ct.c_uint32, ct.c_char_p, ct.c_size_t]),
            "pv_seal": (None, [ct.c_char_p, ct.c_uint, ct.c_uint16, ct.c_size_t]),
            "pv_check": (ct.c_int, [ct.c_char_p, ct.c_size_t]),
            "sim_new": (ct.c_void_p, []),
            "sim_free": (None, [ct.c_void_p]),
            "sim_ring": (ct.c_void_p, [ct.c_void_p]),
            "sim_set_isr": (None, [ct.c_void_p, ct.c_int]),
            "sim_stats": (None, [ct.c_void_p, ct.POINTER(Stats)]),
            "sim_get": (ct.c_int, [ct.c_void_p, ct.c_int]),
            "sim_transfer": (ct.c_uint32, [ct.c_void_p, ct.c_char_p, ct.c_uint32]),
            "ingress_read": (ct.c_size_t, [ct.c_void_p, ct.c_void_p, ct.c_size_t]),
            "ingress_next": (ct.c_void_p, [ct.c_void_p, ct.POINTER(ct.c_size_t),
                                           ct.POINTER(ct.c_uint)]),
        }
        for name, (result, arguments) in api.items():
            fn = getattr(lib, name)
            fn.restype, fn.argtypes = result, arguments
        return self

    def __exit__(self, *args):
        self.tmp.cleanup()


class Ring:
    """One simulated slave: send() is one CS-framed transaction, read() is the consumer."""

    def __init__(self, native):
        self.lib = native.lib
        self.sim = self.lib.sim_new()
        self.ring = self.lib.sim_ring(self.sim)

    def close(self):
        self.lib.sim_free(self.sim)

    def send(self, data):
        return self.lib.sim_transfer(self.sim, data, len(data))

    def read(self, n):
        buf = ct.create_string_buffer(n)
        got = self.lib.ingress_read(self.ring, buf, n)
        return buf.raw[:got]

    def drain(self, chunk=4096):
        out = bytearray()
        while True:
            data = self.read(chunk)
            if not data:
                return bytes(out)
            out += data

    def next(self):
        size, kind = ct.c_size_t(), ct.c_uint()
        p = self.lib.ingress_next(self.ring, ct.byref(size), ct.byref(kind))
        return None if not p else (ct.string_at(p, size.value), kind.value)

    def stats(self):
        s = Stats()
        self.lib.sim_stats(self.sim, ct.byref(s))
        return {name: getattr(s, name) for name in STATS}

    @property
    def ready(self):
        return self.lib.sim_get(self.sim, 0)

    @property
    def violations(self):
        return self.lib.sim_get(self.sim, 1)


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def crc_cases(lib):
    check(lib.pv_crc32(0, b"123456789", 9) == 0xCBF43926, "CRC-32 check value")
    rng = random.Random(1)
    data = bytes(rng.getrandbits(8) for _ in range(4096))
    for start in range(4):
        for n in (0, 1, 3, 4, 5, 63, 1328, 2000):
            got = lib.pv_crc32(0, data[start:], n)
            check(got == zlib.crc32(data[start:start + n]), f"CRC start {start} length {n}")
    check(lib.pv_crc32(lib.pv_crc32(0, data, 100), data[100:], 900) == zlib.crc32(data[:1000]),
          "CRC continuation")


def test_crc(lib):
    crc_cases(lib)
    table = (ct.c_uint32 * 1024)()
    lib.pv_crc32_tables(table)
    crc_cases(lib)
    lib.pv_crc32_tables(None)
    crc_cases(lib)


def test_message(lib):
    # The PV-SPI v1 test vector: one TS packet, PID 0x100, payload i & 0xff, seq 1.
    packet = bytes([0x47, 0x01, 0x00, 0x10]) + bytes(i & 0xFF for i in range(184))
    msg = ct.create_string_buffer(MSG)
    ct.memmove(ct.addressof(msg) + HEADER, packet, len(packet))
    lib.pv_seal(msg, TS, 1, len(packet))
    check(msg.raw == pv_message(1, packet), "pv_seal matches the reference message")
    check(msg.raw[:16].hex(" ") == "50 56 01 01 01 00 bc 00 00 00 00 00 47 01 00 10",
          "PV-SPI v1 vector head")
    check(msg.raw[-4:] == bytes.fromhex("8d32be51"), "PV-SPI v1 vector CRC 0x51be328d")

    guard = ct.create_string_buffer(b"x" * (MSG + 8), MSG + 8)
    lib.pv_seal(guard, GCS, 0, PAYLOAD + 1)
    check(guard.raw == b"x" * (MSG + 8), "oversize seal cannot overrun its buffer")
    reserved = bytearray(pv_message(0, b"x", GCS)); reserved[8] = 1
    reserved[-4:] = zlib.crc32(reserved[:-4]).to_bytes(4, "little")
    check(lib.pv_check(bytes(reserved), MSG) == 3, "nonzero reserved field with good CRC")
    good = pv_message(7, ts_payload(random.Random(2)))
    cases = {
        "ok": (good, 0), "nop": (pv_message(3, b"", NOP), 0),
        "gcs": (pv_message(3, b"x" * 1000, GCS), 0),
        "short": (good[:-1], 1), "long": (good + b"\0", 2),
        "magic": (b"PW" + good[2:], 3), "version": (good[:2] + b"\2" + good[3:], 3),
        "type": (pv_message(3, b"x" * 188, 9), 3),
        "ts length": (pv_message(3, b"\x47" * 100), 3),
        "nop length": (pv_message(3, b"x", NOP), 3),
        "crc": (good[:-1] + bytes([good[-1] ^ 1]), 4),
        "payload bit": (good[:200] + bytes([good[200] ^ 4]) + good[201:], 4),
        "sync": (pv_message(3, b"\x47" + bytes(187) + b"\x46" + bytes(187)), 5),
    }
    for name, (data, want) in cases.items():
        check(lib.pv_check(data, len(data)) == want, f"pv_check {name}")


def sender(rng, count, first=0):
    payloads = [ts_payload(rng) for _ in range(count)]
    return payloads, [pv_message(first + k, p) for k, p in enumerate(payloads)]


def test_stream(native):
    """Clean stream, consumer reading random amounts between random bursts of messages."""
    rng = random.Random(3)
    payloads, messages = sender(rng, 2000, first=65000)  # crosses the 16-bit seq wrap
    ring, out, sent = Ring(native), bytearray(), 0
    while sent < len(messages):
        burst = rng.randint(1, 20)
        while burst and sent < len(messages) and ring.ready:
            check(ring.send(messages[sent]) == MSG, "a sender obeying READY is never dropped")
            sent, burst = sent + 1, burst - 1
        for _ in range(rng.randint(0, 6)):
            out += ring.read(rng.choice((1, 188, 1316, 5370, 9000)))
    out += ring.drain()
    s = ring.stats()
    check(bytes(out) == b"".join(payloads), "byte stream equals the payloads in order")
    check(s["msgs"] == 2000 and s["bytes"] == 2000 * PAYLOAD, "all messages accepted")
    check(all(s[k] == 0 for k in STATS if k not in ("msgs", "bytes", "ready_falls", "ts_packets", "crc_chain")),
          f"no errors, losses or overruns: {s}")
    chain = 0
    for m in messages: chain = zlib.crc32(m[-4:], chain)
    check(s["ts_packets"] == 14000 and s["crc_chain"] == chain, "PV sender counters and chain")
    check(ring.violations == 0, "the DMA never wrote a consumer-owned slot")
    ring.close()


def test_ready(native):
    """READY falls when fewer than MARGIN slots are armed and rises after a release."""
    rng = random.Random(4)
    _, messages = sender(rng, SLOTS + 4)
    ring = Ring(native)
    check(ring.ready == 1, "READY high once armed")
    for k in range(SLOTS - MARGIN + 1):
        check(ring.ready == 1, f"READY high with {SLOTS - k} armed slots")
        ring.send(messages[k])
    check(ring.ready == 0, f"READY low with {MARGIN - 1} armed slot")
    # One late sender: it sampled READY high before the last end-of-frame was seen.
    check(ring.send(messages[SLOTS - MARGIN + 1]) == MSG, "the margin slot holds a late message")
    check(ring.stats()["overruns"] == 0, "no overrun from one late message")
    out = ring.read(PAYLOAD)
    check(len(out) == PAYLOAD and ring.ready == 0, "one release is not yet enough")
    ring.read(PAYLOAD)
    check(ring.ready == 1, "READY high again after two releases")
    check(ring.stats()["ready_falls"] == 1, "one READY fall counted")
    ring.close()


def test_late_sampling(native):
    """A sender that samples READY only before every second message never overruns."""
    rng = random.Random(5)
    payloads, messages = sender(rng, 3000)
    ring, out, sent = Ring(native), bytearray(), 0
    while sent < len(messages):
        if ring.ready:
            for m in messages[sent:sent + 2]:
                ring.send(m)
            sent += 2
        if rng.random() < 0.3:
            out += ring.read(rng.choice((1316, 2632)))
    out += ring.drain()
    s = ring.stats()
    check(s["overruns"] == 0 and s["lost"] == 0 and bytes(out) == b"".join(payloads),
          f"margin of {MARGIN} absorbs one unchecked message: {s}")
    ring.close()


def test_faults(native):
    """Bad CRC, short, long, header and sync errors are dropped, counted and do not desync."""
    rng = random.Random(6)
    payloads, messages = sender(rng, 401)   # the last one is clean: losses at the very end
    sent, kept = [], []                       # are invisible to sequence counting
    for k, m in enumerate(messages):
        kind = k % 8 if k % 4 == 3 else -1
        if kind == 3:
            m = m[:200] + bytes([m[200] ^ 1]) + m[201:]   # CRC
        elif kind == 7 and k % 16 == 7:
            m = m[:1000]                                   # short
        elif kind == 7 and k % 32 == 15:
            m = m + bytes(64)                              # long, spills into the next slot
        elif kind == 7 and k % 32 == 31:
            m = m + bytes(SLOT_BYTES - MSG)                # long, fills its slot exactly
        else:
            kept.append(payloads[k])
        sent.append(m)
    ring, out = Ring(native), bytearray()
    for m in sent:
        check(ring.ready == 1, "READY high while the consumer keeps up")
        ring.send(m)
        out += ring.read(PAYLOAD)
    out += ring.drain()
    s = ring.stats()
    want = {"err_crc": 50, "err_short": 25, "err_long": 25, "lost": 100,
            "msgs": 301, "overruns": 0}
    check(all(s[k] == v for k, v in want.items()), f"fault counts {s}")
    check(bytes(out) == b"".join(kept), "only intact messages reach the stream")
    chain = 0
    for seq, payload in enumerate(payloads):
        if payload in kept:
            chain = zlib.crc32(pv_message(seq, payload)[-4:], chain)
    check(s["crc_chain"] == chain and s["ts_packets"] == 7*len(kept),
          "rejected messages do not enter accepted TS counters or CRC chain")
    check(ring.violations == 0, "ownership kept through faults")
    ring.close()


def test_overrun(native):
    """Withheld credit and a sender that ignores READY: data is lost, never corrupted."""
    rng = random.Random(7)
    payloads, messages = sender(rng, 600)
    ring, out = Ring(native), bytearray()
    for k, m in enumerate(messages):
        ring.send(m)                       # ignores READY
        if k % 50 >= 30:                   # consumer withholds 20 of every 50 messages
            out += ring.read(rng.choice((1316, 2632)))
    out += ring.drain()
    s = ring.stats()
    check(s["overruns"] > 0 and s["restarts"] > 0, f"overruns detected and recovered: {s}")
    check(s["msgs"] + s["lost"] == 600, f"every message accepted or counted lost: {s}")
    delivered = [bytes(out[i:i + PAYLOAD]) for i in range(0, len(out), PAYLOAD)]
    check(len(out) % PAYLOAD == 0 and set(delivered) <= set(payloads), "no corrupt payload")
    check(delivered == [p for p in payloads if p in set(delivered)], "order kept")
    check(ring.violations == 0, "ownership kept through overruns")
    ring.close()


def test_next(native):
    """Zero-copy path, NOP and GCS messages, backwards sequence numbers."""
    ring = Ring(native)
    ring.send(pv_message(10, b"\x47" + b"a" * 187, TS))
    ring.send(pv_message(11, b"", NOP))
    ring.send(pv_message(12, b"generic bytes", GCS))
    ring.send(pv_message(5, b"\x47" + b"b" * 187, TS))
    check(ring.next() == (b"\x47" + b"a" * 187, TS), "TS message, zero copy")
    check(ring.next() == (b"generic bytes", GCS), "NOP skipped, GCS delivered")
    check(ring.next() is None, "backwards sequence is dropped")
    check(ring.next() is None, "empty")
    s = ring.stats()
    check(s["nops"] == 1 and s["seq_back"] == 1 and s["lost"] == 0, f"next counters {s}")
    ring.close()


def native_checks():
    with Native() as native:
        test_crc(native.lib)
        test_message(native.lib)
        for test in (test_stream, test_ready, test_late_sampling, test_faults, test_overrun,
                     test_next):
            test(native)
    print("ingress: CRC, PV format, ring ownership, READY margin, faults, overrun: ok")



def test_type(native):
    ring = Ring(native)
    native.lib.sim_type(ring.sim, TS)
    ring.send(pv_message(0, b"generic", GCS))
    ring.send(pv_message(1, b"\x47" + bytes(187), TS))
    assert ring.drain() == b"\x47" + bytes(187)
    assert native.lib.sim_wrong_type(ring.sim) == 1
    ring.send(pv_message(1, b"\x47" + bytes(187), TS))
    assert ring.drain() == b"" and ring.stats()["seq_back"] == 1
    ring.close()


def test_pattern(lib):
    sys.path.insert(0, str(ROOT / "host"))
    from spi_input import pattern
    for seq in (0, 1, 63, 255, 256, 65535):
        for kind in (TS, GCS):
            p = ct.create_string_buffer(PAYLOAD)
            lib.ingress_pattern(p, seq, kind)
            assert p.raw == pattern(seq, kind)


if __name__ == "__main__":
    native_checks()
    with Native() as native:
        test_type(native)
        test_pattern(native.lib)
    print("ingress: type lock, duplicate rejection and independent sender pattern: ok")
