"""Check the W5500 UDP driver against a register-level fake chip on the host.

    python3 tests/test_w5500.py

Compiles firmware/components/w5500 (w5500.c, w5500_settings.c) with tests/w5500_fake.c, a model of
the W5500's SPI frames, registers, buffer memories and UDP sockets (W5500 Datasheet v1.1.0), and
drives it with ctypes. No ESP-IDF, board or pytest. The fake counts every frame and keeps a clock
from the same transaction costs as w5500.h, so the throughput table in that header is checked
against the frames the driver really issues.
"""
import ctypes as ct
import os
import pathlib
import random
import shlex
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "firmware/components/w5500"

OK, FULL, ERR_ARG, ERR_BUS, ERR_CHIP, ERR_SOCKET = 0, 1, -1, -2, -3, -4
UDP_MAX, TXQ, SMALL_MAX = 1472, 16, 32
TS_DATAGRAM = 1316
T_POLL, T_QUEUE = 11.0, 26.0          # us per polled / interrupt transaction (w5500.h)
NET = dict(gateway=[192, 168, 77, 1], mask=[255, 255, 255, 0],
           mac=[0x02, 0x00, 0x00, 0x00, 0x55, 0x00], ip=[192, 168, 77, 2])
LAPTOP = bytes([192, 168, 77, 1])
BROADCAST = bytes([255] * 4)
SN_MR, SN_SR, SN_PORT, SN_RX_WR = 0x00, 0x03, 0x04, 0x2A

STATS = ("sent", "sent_bytes", "full", "dropped", "received", "received_bytes", "overruns",
         "discarded", "malformed", "deferred")
FAKE = ("frames", "polled", "queued", "bytes", "violations", "rx_dropped", "arp_requests",
        "resets", "logged")


class Stats(ct.Structure):
    _fields_ = [(name, ct.c_uint32) for name in STATS]


class FakeStats(ct.Structure):
    _fields_ = [(name, ct.c_uint32) for name in FAKE] + [("now_us", ct.c_double),
                                                          ("cpu_us", ct.c_double)]


class Peer(ct.Structure):
    _fields_ = [("ip", ct.c_uint8 * 4), ("port", ct.c_uint16)]


class Settings(ct.Structure):
    _fields_ = [("ip", ct.c_uint8 * 4), ("mask", ct.c_uint8 * 4), ("gateway", ct.c_uint8 * 4),
                ("dest", ct.c_uint8 * 4), ("port", ct.c_uint16), ("dest_port", ct.c_uint16),
                ("sck_khz", ct.c_uint16), ("sclk", ct.c_int8), ("mosi", ct.c_int8),
                ("miso", ct.c_int8), ("cs", ct.c_int8)]


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def control(bsb, write):
    """Independent control byte (DS 2.2.2, pp.15-17): BSB[4:0], RWB, OM = 00."""
    return bsb << 3 | (4 if write else 0)


def reg(s): return 4 * s + 1
def txbuf(s): return 4 * s + 2
def rxbuf(s): return 4 * s + 3


class Native:
    def __enter__(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="w5500-")
        library = pathlib.Path(self.tmp.name) / "w5500.so"
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-fPIC", "-shared",
            "-I", str(COMPONENT / "include"), str(COMPONENT / "w5500.c"),
            str(COMPONENT / "w5500_settings.c"), str(ROOT / "tests/w5500_fake.c"),
            "-o", str(library)], check=True)
        lib = self.lib = ct.CDLL(str(library))
        p, u16, b = ct.c_void_p, ct.c_uint16, ct.c_char_p
        api = {
            "fake_new": (p, []), "fake_free": (None, [p]),
            "fake_set": (ct.c_int, [p, b, ct.c_double]), "fake_why": (b, [p]),
            "fake_get_stats": (None, [p, ct.POINTER(FakeStats)]),
            "fake_advance": (None, [p, ct.c_double]),
            "fake_inject": (ct.c_int, [p, b, u16, u16, b, ct.c_size_t]),
            "fake_tear": (None, [p, b, u16, u16, b, ct.c_size_t]),
            "fake_inject_split": (ct.c_int, [p, b, u16, u16, b, ct.c_size_t, ct.c_uint,
                                             ct.c_double]),
            "fake_take": (ct.c_int, [p, p, ct.POINTER(u16), ct.POINTER(u16), p]),
            "fake_peek": (ct.c_int, [p, ct.c_uint, ct.c_uint]),
            "fake_poke": (None, [p, ct.c_uint, ct.c_uint, ct.c_uint]),
            "fake_physical": (ct.c_int, [p, ct.c_uint, ct.c_int]),
            "fake_trace": (ct.c_int, [p, ct.c_uint, ct.POINTER(u16), ct.POINTER(ct.c_uint8),
                                      ct.POINTER(u16)]),
            "fake_trace_count": (ct.c_uint, [p]),
            "fake_frame_raw": (ct.c_int, [p, u16, ct.c_uint8, b, p, ct.c_size_t]),
            "fake_driver_init": (ct.c_int, [p, b, b]), "fake_driver": (p, [p]),
            "fake_driver_counters": (None, [p, ct.POINTER(ct.c_uint32 * 3)]),
            "fake_driver_size": (ct.c_size_t, []),
            "w5500_phy": (ct.c_int, [p]),
            "w5500_udp_open": (ct.c_int, [p, ct.c_uint, u16]),
            "w5500_udp_close": (ct.c_int, [p, ct.c_uint]),
            "w5500_udp_dest": (None, [p, ct.c_uint, b, u16]),
            "w5500_udp_send": (ct.c_int, [p, ct.c_uint, b, ct.c_size_t]),
            "w5500_udp_poll": (ct.c_int, [p, ct.c_uint]),
            "w5500_udp_recv": (ct.c_int, [p, ct.c_uint, p, ct.c_size_t, ct.POINTER(Peer)]),
            "w5500_udp_stats": (ct.POINTER(Stats), [p, ct.c_uint]),
            "w5500_settings_default": (None, [ct.POINTER(Settings)]),
            "w5500_settings_parse": (ct.c_int, [ct.POINTER(Settings), b]),
            "w5500_settings_json": (ct.c_int, [ct.POINTER(Settings), b, ct.c_size_t]),
        }
        for name, (result, arguments) in api.items():
            fn = getattr(lib, name)
            fn.restype, fn.argtypes = result, arguments
        return self

    def __exit__(self, *args):
        self.tmp.cleanup()


class Chip:
    """One fake W5500 and the driver wired to it."""

    def __init__(self, native, **settings):
        self.lib = native.lib
        self.f = self.lib.fake_new()
        self.w = self.lib.fake_driver(self.f)
        self.set(**settings)

    def close(self):
        check(self.fake()["violations"] == 0, f"violation: {self.why()}")
        self.lib.fake_free(self.f)

    def set(self, **settings):
        for key, value in settings.items():
            check(self.lib.fake_set(self.f, key.encode(), float(value)) == 0, key)

    def why(self):
        return self.lib.fake_why(self.f).decode()

    def init(self, tx0=0, rx0=0, tx1=0, rx1=0, net=NET):
        raw = bytes(net["gateway"] + net["mask"] + net["mac"] + net["ip"])
        return self.lib.fake_driver_init(self.f, raw, bytes([tx0, rx0, tx1, rx1]))

    def fake(self):
        s = FakeStats()
        self.lib.fake_get_stats(self.f, ct.byref(s))
        return {name: getattr(s, name) for name, _ in FakeStats._fields_}

    def stats(self, s=0):
        st = self.lib.w5500_udp_stats(self.w, s).contents
        return {name: getattr(st, name) for name in STATS}

    def counters(self):
        out = (ct.c_uint32 * 3)()
        self.lib.fake_driver_counters(self.f, ct.byref(out))
        return dict(zip(("frames", "frame_bytes", "bus_errors"), out))

    def open(self, s, port):
        return self.lib.w5500_udp_open(self.w, s, port)

    def dest(self, s, ip, port):
        self.lib.w5500_udp_dest(self.w, s, bytes(ip), port)

    def send(self, s, data):
        return self.lib.w5500_udp_send(self.w, s, data, len(data))

    def poll(self, s):
        return self.lib.w5500_udp_poll(self.w, s)

    def recv(self, s, cap=UDP_MAX):
        buf, peer = ct.create_string_buffer(max(cap, 1)), Peer()
        n = self.lib.w5500_udp_recv(self.w, s, buf, cap, ct.byref(peer))
        return n, (buf.raw[:n] if n > 0 else b""), (bytes(peer.ip), peer.port)

    def inject(self, data, ip=LAPTOP, sport=40000, dport=5004):
        return self.lib.fake_inject(self.f, bytes(ip), sport, dport, data, len(data))

    def inject_split(self, data, first, delay_us, ip=LAPTOP, sport=40000, dport=5004):
        return self.lib.fake_inject_split(self.f, bytes(ip), sport, dport, data, len(data), first,
                                          delay_us)

    def tear(self, data, ip=LAPTOP, sport=40000, dport=5004):
        self.lib.fake_tear(self.f, bytes(ip), sport, dport, data, len(data))

    def take(self):
        ip, port, sport = ct.create_string_buffer(4), ct.c_uint16(), ct.c_uint16()
        data = ct.create_string_buffer(UDP_MAX)
        n = self.lib.fake_take(self.f, ip, ct.byref(port), ct.byref(sport), data)
        return None if n < 0 else (ip.raw, port.value, sport.value, data.raw[:n])

    def drain(self, s, limit_us=5e6):
        """Advance time until everything staged is on the wire; return the wire's datagrams."""
        waited = 0.0
        while self.poll(s) > 0:
            check(waited < limit_us, "send queue never drained")
            self.lib.fake_advance(self.f, 50.0)
            waited += 50.0
        out = []
        while (d := self.take()) is not None:
            out.append(d)
        return out

    def peek(self, bsb, offset):
        return self.lib.fake_peek(self.f, bsb, offset)

    def poke(self, bsb, offset, value):
        self.lib.fake_poke(self.f, bsb, offset, value)

    def peek16(self, s, offset):
        return self.peek(reg(s), offset) << 8 | self.peek(reg(s), offset + 1)

    def poke16(self, s, offset, value):
        self.poke(reg(s), offset, value >> 8)
        self.poke(reg(s), offset + 1, value & 0xFF)

    def trace(self, first):
        """Frames issued since trace count `first`: (offset, control, n)."""
        out, o, c, n = [], ct.c_uint16(), ct.c_uint8(), ct.c_uint16()
        for i in range(first, self.lib.fake_trace_count(self.f)):
            check(self.lib.fake_trace(self.f, i, ct.byref(o), ct.byref(c), ct.byref(n)) == 0,
                  "trace window")
            out.append((o.value, c.value, n.value))
        return out

    def mark(self):
        return self.lib.fake_trace_count(self.f)


def sender(native, tx_kb=16, port=5004, **settings):
    chip = Chip(native, **settings)
    check(chip.init(tx0=tx_kb) == OK, "init")
    check(chip.open(0, port) == OK, "open")
    chip.dest(0, BROADCAST, 5006)
    return chip


def receiver(native, rx_kb=16, port=5004, **settings):
    chip = Chip(native, **settings)
    check(chip.init(rx0=rx_kb) == OK, "init")
    check(chip.open(0, port) == OK, "open")
    return chip


def payload(rng, n):
    return bytes(rng.getrandbits(8) for _ in range(n))


def ts_datagram(seq):
    """7 TS packets: 0x47, PID 0x100, continuity counter, then a counting payload."""
    out = bytearray()
    for k in range(7):
        cc = (7 * seq + k) & 15
        out += bytes([0x47, 0x01, 0x00, 0x10 | cc])
        out += bytes((seq + k + i) & 0xFF for i in range(184))
    return bytes(out)


def test_frame_format(native):
    """Control bytes follow the datasheet's block-select table and worked examples."""
    # DS p.19 SIMR write, p.20 socket 1 TX buffer write, p.22 S7_SR read, p.23 socket 3 RX read.
    check([control(0, 1), control(txbuf(1), 1), control(reg(7), 0), control(rxbuf(3), 0)] ==
          [0x04, 0x34, 0xE8, 0x78], "control byte examples")
    chip = Chip(native)
    check(chip.init(tx0=4, rx0=4, tx1=8, rx1=2) == OK, "init")
    frames = chip.trace(0)
    check(frames[0] == (0x0039, control(0, 0), 1), "VERSIONR read first")
    check(frames[1] == (0x0000, control(0, 1), 1), "MR software reset")
    check((0x0001, control(0, 1), 18) in frames and (0x0001, control(0, 0), 18) in frames,
          "GAR..SIPR written and read back as one run")
    sizes = [f for f in frames if f[0] == 0x001E]
    check(sizes == [(0x001E, control(reg(s), 1), 2) for s in range(8)], "all eight sockets sized")
    check([chip.peek(reg(s), 0x1F) for s in range(8)] == [4, 8] + [0] * 6, "TX sizes")
    check([chip.peek(reg(s), 0x1E) for s in range(8)] == [4, 2] + [0] * 6, "RX sizes")
    check([chip.lib.fake_physical(chip.f, 1, tx) for tx in (1, 0)] == [4096, 4096],
          "socket 1 buffers follow socket 0's (DS p.31)")
    raw = [chip.peek(0, a) for a in range(1, 19)]
    check(raw == NET["gateway"] + NET["mask"] + NET["mac"] + NET["ip"], "GAR, SUBR, SHAR, SIPR")
    check(chip.open(1, 6000) == OK, "open socket 1")
    chip.dest(1, BROADCAST, 6001)
    first = chip.mark()
    check(chip.send(1, b"hello") == OK, "send on socket 1")
    frames = chip.trace(first)
    check(frames[0] == (0x0000, control(txbuf(1), 1), 5), "socket 1 TX buffer write (DS p.20)")
    check(chip.drain(1) == [(BROADCAST, 6001, 6000, b"hello")], "socket 1 datagram")
    chip.close()


def test_fake(native):
    """The fake's own checks fire, so a clean run means something."""
    lib = native.lib
    chip = sender(native, tx_kb=4)

    def frame(offset, ctl, data=None, n=1):
        rx = ct.create_string_buffer(n)
        lib.fake_frame_raw(chip.f, offset, ctl, data, None if data else rx,
                           len(data) if data else n)
        return rx.raw

    def fires(reason, action):
        before = chip.fake()["violations"]
        action()
        check(chip.fake()["violations"] == before + 1 and reason in chip.why(),
              f"fake flags {reason!r}: got {chip.why()!r}")

    check(chip.send(0, bytes(1000)) == OK, "a SEND in progress")
    fires("SEND while a SEND", lambda: frame(0x01, control(reg(0), 1), b"\x20"))
    fires("over unsent data", lambda: frame(chip.peek16(0, 0x22), control(txbuf(0), 1), b"x"))
    fires("destination changed", lambda: frame(0x0C, control(reg(0), 1), b"\x0a"))
    fires("reserved block", lambda: frame(0, control(4, 0)))
    fires("OM 1", lambda: frame(0x39, 1))
    fires("read-only", lambda: frame(0x03, control(reg(0), 1), b"\x00"))
    fires("RX buffer access with no memory", lambda: frame(0, control(rxbuf(0), 0)))
    chip.set(cmd_us=100)
    frame(0x01, control(reg(1), 1), b"\x10")
    fires("before 0x10 was accepted", lambda: frame(0x01, control(reg(1), 1), b"\x10"))
    lib.fake_free(chip.f)

    chip = receiver(native)
    chip.inject(bytes(10))
    fires("past Sn_RX_WR", lambda: frame(chip.peek16(0, SN_RX_WR), control(rxbuf(0), 0)))
    fires("beyond Sn_RX_WR", lambda: (frame(0x28, control(reg(0), 1), b"\xff\xff", 2),
                                      frame(0x01, control(reg(0), 1), b"\x40")))
    lib.fake_free(chip.f)


def test_init(native):
    chip = Chip(native, version=0)
    check(chip.init() == ERR_CHIP and chip.counters()["frames"] == 1, "VERSIONR 0 means no chip")
    chip.close()
    chip = Chip(native, fail_in=0)
    check(chip.init() == ERR_BUS and chip.counters()["bus_errors"] == 1, "bus failure")
    chip.close()
    for sizes in ((3, 0, 0, 0), (16, 0, 1, 0), (0, 8, 0, 16), (0, 32, 0, 0)):
        chip = Chip(native)
        check(chip.init(*sizes) == ERR_ARG and chip.counters()["frames"] == 0, f"sizes {sizes}")
        chip.close()
    chip = Chip(native, mosi_and=0xFE)
    check(chip.init() == ERR_CHIP, "a stuck MOSI bit fails the read-back")
    chip.close()
    chip = Chip(native, reset_us=1000)
    check(chip.init(tx0=16) == OK and chip.fake()["resets"] == 1, "slow reset is waited for")
    chip.close()
    chip = Chip(native, reset_us=1e9)
    check(chip.init() == ERR_CHIP, "reset that never finishes")
    chip.close()
    chip = Chip(native, phy=0)
    check(chip.init(rx0=16) == OK and chip.lib.w5500_phy(chip.w) == 0xB8, "link down, reset value")
    chip.set(phy=7)
    check(chip.lib.w5500_phy(chip.w) & 7 == 7, "link up, 100 Mb/s, full duplex")
    chip.close()


def test_open(native):
    chip = Chip(native, open_ptr=0xFFF0, cmd_us=50)
    check(chip.init(tx0=8, rx0=8) == OK, "init")
    check(chip.open(2, 1) == ERR_ARG, "only sockets 0 and 1")
    check(chip.open(0, 5004) == OK, "open with slow command acceptance")
    check(chip.peek(reg(0), SN_MR) == 0x02 and chip.peek16(0, SN_PORT) == 5004 and
          chip.peek(reg(0), SN_SR) == 0x22, "UDP mode, port, SOCK_UDP")
    chip.dest(0, BROADCAST, 5006)
    rng = random.Random(1)
    data = [payload(rng, 1000) for _ in range(3)]
    for d in data:
        check(chip.send(0, d) == OK, "send near the 16-bit pointer wrap")
    check([d[3] for d in chip.drain(0)] == data, "pointers taken from the chip after OPEN")
    check(chip.lib.w5500_udp_close(chip.w, 0) == OK and chip.peek(reg(0), SN_SR) == 0, "close")
    check(chip.send(0, b"x") == ERR_ARG, "send on a closed socket")
    chip.close()


def test_send(native):
    rng = random.Random(2)
    chip = sender(native, open_ptr=0xFF00)
    sizes = [TS_DATAGRAM] * 40 + [rng.randint(1, UDP_MAX) for _ in range(150)] + [1, UDP_MAX]
    data = [payload(rng, n) for n in sizes]
    for d in data:
        check(chip.send(0, d) == OK, "back-to-back sends never wait on a 100 Mb/s wire")
    out = chip.drain(0)
    check([d[3] for d in out] == data, "datagrams on the wire in order and intact")
    check(all(d[:3] == (BROADCAST, 5006, 5004) for d in out), "destination and source port")
    st = chip.stats()
    check(st["sent"] == len(data) and st["sent_bytes"] == sum(sizes) and st["full"] == 0,
          "send counters")
    check(sum(sizes) > 2 * 65536, "TX pointers wrapped the 16-bit range twice")
    for bad in (b"", bytes(UDP_MAX + 1)):
        check(chip.send(0, bad) == ERR_ARG, f"length {len(bad)}")
    check(chip.lib.w5500_udp_recv(chip.w, 0, None, 0, None) == ERR_ARG, "no RX buffer")
    chip.close()

    chip = sender(native, tx_kb=1)
    check(chip.send(0, bytes(1025)) == ERR_ARG, "longer than the TX buffer")
    check(chip.send(0, bytes(1024)) == OK, "exactly the TX buffer")
    chip.drain(0)
    chip.close()

    chip = sender(native)
    chip.set(fail_in=0)
    check(chip.send(0, b"x" * 100) == ERR_BUS and chip.counters()["bus_errors"] == 1, "bus error")
    chip.close()


def test_full(native):
    rng = random.Random(3)
    # 2 KB buffer on a 10 Mb/s link: the second datagram finds the first still on the wire.
    chip = sender(native, tx_kb=2, wire_bps=10e6)
    first, second = payload(rng, TS_DATAGRAM), payload(rng, TS_DATAGRAM)
    check(chip.send(0, first) == OK, "first")
    before = chip.fake()["bytes"]
    check(chip.send(0, second) == FULL and chip.stats()["full"] == 1, "second is refused")
    check(chip.fake()["bytes"] - before == 6, "a refusal costs one 3-byte status read")
    chip.lib.fake_advance(chip.f, 2000.0)
    check(chip.send(0, second) == OK, "accepted once the wire is free")
    check([d[3] for d in chip.drain(0)] == [first, second], "both delivered")
    chip.close()

    # ARP stalls the first SEND: the 16 KB buffer stages 11 more 1,316-byte datagrams.
    chip = sender(native, arp_us=50000)
    chip.dest(0, LAPTOP, 5006)
    data, n = [], 0
    while True:
        d = payload(rng, TS_DATAGRAM)
        r = chip.send(0, d)
        if r == FULL:
            break
        check(r == OK, "staged")
        data.append(d)
    check(len(data) == 12 and chip.poll(0) == 12, "1 waiting for ARP + 11 staged = 16,384 // 1,316")
    out = chip.drain(0)
    check([d[3] for d in out] == data and all(d[0] == LAPTOP for d in out), "all delivered")
    check(chip.fake()["arp_requests"] == 1, "one ARP for the destination")
    chip.close()

    # Small datagrams run out of queue entries before buffer bytes.
    chip = sender(native, arp_us=50000)
    chip.dest(0, LAPTOP, 5006)
    accepted = 0
    while chip.send(0, bytes([accepted]) * 100) == OK:
        accepted += 1
    check(accepted == 1 + TXQ, "one in flight plus W5500_TXQ staged")
    check(len(chip.drain(0)) == accepted, "queue drained")
    chip.close()


def test_arp_timeout(native):
    chip = sender(native, arp_reply=0)
    chip.dest(0, LAPTOP, 5006)
    for k in range(3):
        check(chip.send(0, bytes([k]) * 500) == OK, "staged behind the ARP")
    check(chip.poll(0) == 3, "nothing sent yet")
    chip.lib.fake_advance(chip.f, 1.79e6)
    check(chip.poll(0) == 3, "ARPTO = 2000 x 0.1 ms x (8 + 1) = 1.8 s (DS p.38-39)")
    chip.lib.fake_advance(chip.f, 0.02e6)
    check(chip.poll(0) == 0 and chip.stats()["dropped"] == 3, "TIMEOUT drops the queue")
    check(chip.take() is None, "nothing reached the wire")
    chip.set(arp_reply=1)
    chip.dest(0, BROADCAST, 5006)
    check(chip.send(0, b"after") == OK, "the socket was reopened")
    check([d[3] for d in chip.drain(0)] == [b"after"], "broadcast after the timeout")
    chip.close()


def steady_state(chip, step, count):
    """Per-operation frames, bytes, time and CPU over `count` repetitions of step()."""
    a = chip.fake()
    for k in range(count):
        step(k)
    b = chip.fake()
    keys = ("polled", "queued", "bytes", "now_us", "cpu_us")
    return {key: (b[key] - a[key]) / count for key in keys}


def test_throughput(native):
    """The w5500.h derivation, checked frame by frame."""
    table = {}
    for mhz in (40.0, 80 / 3, 20.0, 10.0):
        f = mhz * 1e6
        chip = sender(native, sck_hz=f)
        data = ts_datagram(0)
        for k in range(5):                                  # reach the steady state
            check(chip.send(0, data) == OK, "warm-up")
        s = steady_state(chip, lambda k: check(chip.send(0, data) == OK, "send"), 50)
        check(s["polled"] == 4 and s["queued"] == 1 and s["bytes"] == 1338,
              f"send frames at {mhz:.2f} MHz: {s}")
        t_send = T_QUEUE + 4 * T_POLL + 8 * 1338 / mhz
        check(abs(s["now_us"] - t_send) < 0.01, f"send time {s['now_us']} vs {t_send}")
        cpu_send = T_QUEUE + 4 * T_POLL + 8 * 19 / mhz
        check(abs(s["cpu_us"] - cpu_send) < 0.01, "send CPU")
        chip.drain(0)
        chip.close()

        chip = receiver(native, sck_hz=f)
        buf = ct.create_string_buffer(UDP_MAX)

        def one(k):
            check(chip.inject(data) == 1, "inject")
            check(chip.lib.w5500_udp_recv(chip.w, 0, buf, UDP_MAX, None) == TS_DATAGRAM, "recv")
        r = steady_state(chip, one, 50)
        check(r["polled"] == 5 and r["queued"] == 1 and r["bytes"] == 1348,
              f"receive frames at {mhz:.2f} MHz: {r}")
        t_recv = T_QUEUE + 5 * T_POLL + 8 * 1348 / mhz
        check(abs(r["now_us"] - t_recv) < 0.01, f"receive time {r['now_us']} vs {t_recv}")
        check(abs(r["cpu_us"] - (T_QUEUE + 5 * T_POLL + 8 * 29 / mhz)) < 0.01, "receive CPU")
        empty = steady_state(chip, lambda k: chip.recv(0), 10)
        check(empty["polled"] == 1 and abs(empty["now_us"] - (T_POLL + 40 / mhz)) < 0.01,
              "an empty poll is one Sn_RX_WR read")
        chip.close()
        table[mhz] = (t_send, 8 * TS_DATAGRAM / t_send, t_recv, 8 * TS_DATAGRAM / t_recv,
                      s["cpu_us"], r["cpu_us"])
    expect = {40.0: (31.2, 30.0), 80 / 3: (22.3, 21.7), 20.0: (17.4, 17.0), 10.0: (9.2, 9.1)}
    for mhz, (send_mbps, recv_mbps) in expect.items():
        check(round(table[mhz][1], 1) == send_mbps and round(table[mhz][3], 1) == recv_mbps,
              f"w5500.h table at {mhz:.2f} MHz")
    default = table[80 / 3]
    check(default[1] >= 15 and default[3] >= 15, "15 Mb/s goal at the default 26.67 MHz")
    per_second = 12.64e6 / (8 * TS_DATAGRAM)
    check(round(100 * per_second * default[4] / 1e6, 1) == 9.1, "send CPU share at 12.64 Mb/s")
    check(round(100 * per_second * default[5] / 1e6, 1) == 10.8, "receive CPU share at 12.64 Mb/s")
    return table


def test_receive(native):
    chip = receiver(native)
    check(chip.recv(0)[0] == 0, "empty")
    check(chip.inject(b"x", dport=5005) == -1, "no socket on that port")
    check(chip.inject(b"hello", ip=[10, 1, 2, 3], sport=1234) == 1, "inject")
    n, data, peer = chip.recv(0)
    check((n, data, peer) == (5, b"hello", (bytes([10, 1, 2, 3]), 1234)), "payload and peer")
    check(chip.peek16(0, 0x28) == chip.peek16(0, SN_RX_WR), "Sn_RX_RD committed")
    check(chip.lib.w5500_udp_send(chip.w, 0, b"x", 1) == ERR_ARG, "no TX buffer")
    chip.close()

    # Back to back: 12 datagrams fill 16 KB, the 13th is dropped by the chip.
    chip = receiver(native)
    data = [ts_datagram(k) for k in range(13)]
    accepted = [chip.inject(d) for d in data]
    check(accepted == [1] * 12 + [0], "the chip keeps 12 x (8 + 1,316) bytes")
    first = chip.mark()
    out = [chip.recv(0)[1] for _ in range(12)]
    check(out == data[:12] and chip.recv(0)[0] == 0, "all 12 in order")
    check(chip.stats()["overruns"] == 1, "the full buffer was seen")
    reads = [f for f in chip.trace(first) if f[0] == SN_RX_WR and f[2] == 2]
    commits = [f for f in chip.trace(first) if f[0] == 0x28]
    check(len(reads) == 2 and len(commits) == 1, "one Sn_RX_WR read and one RECV per batch")
    chip.close()

    # Random sizes and interleaving across the 16-bit pointer wrap.
    rng = random.Random(4)
    chip = receiver(native, open_ptr=0xFFF0)
    sent, got, total = [], [], 0
    while len(sent) < 400:
        for _ in range(rng.randint(0, 5)):
            d = payload(rng, rng.randint(1, UDP_MAX))
            r = chip.inject(d, sport=len(sent))
            if r == 1:
                sent.append(d)
                total += 8 + len(d)
        for _ in range(rng.randint(0, 4)):
            n, d, peer = chip.recv(0)
            if n:
                check(peer == (LAPTOP, len(got)), "peer port in order")
                got.append(d)
    while (r := chip.recv(0))[0]:
        got.append(r[1])
    check(got == sent and total > 4 * 65536, "every datagram intact across 16-bit wraps")
    st = chip.stats()
    check(st["received"] == len(sent) and st["malformed"] == 0 and st["discarded"] == 0,
          f"counters {st}")
    chip.close()


def test_malformed(native):
    good = ts_datagram(9)
    # A header length beyond what Sn_RX_WR covers is first waited for, then flushed.
    chip = receiver(native)
    chip.inject(bytes(100))
    rd = chip.peek16(0, 0x28)
    chip.poke(rxbuf(0), (rd + 6) & 0xFFFF, 0)
    chip.poke(rxbuf(0), (rd + 7) & 0xFFFF, 200)
    before = chip.fake()["now_us"]
    check(chip.recv(0)[0] == 0 and chip.stats()["malformed"] == 1, "length past Sn_RX_WR")
    check(chip.fake()["now_us"] - before >= 2475, "flushed only after 2.5 ms of stable reads")
    chip.inject(good)
    check(chip.recv(0)[1] == good, "next datagram after the flush")
    chip.close()

    # A zero-length datagram and one longer than the caller's buffer are skipped in one call.
    chip = receiver(native)
    chip.inject(b"")
    chip.inject(bytes(1400))
    chip.inject(good)
    n, data, _ = chip.recv(0, cap=TS_DATAGRAM)
    check(data == good and chip.stats()["discarded"] == 2, "empty and oversize skipped")
    chip.close()

    # A length no UDP datagram can have is flushed at once.
    chip = receiver(native)
    chip.inject(bytes(100))
    rd = chip.peek16(0, 0x28)
    chip.poke(rxbuf(0), (rd + 6) & 0xFFFF, 0x06)
    chip.poke(rxbuf(0), (rd + 7) & 0xFFFF, 0x00)
    check(chip.recv(0)[0] == 0 and chip.stats()["malformed"] == 1, "length 1,536")
    chip.inject(good)
    check(chip.recv(0)[1] == good, "next datagram after the flush")
    chip.close()

    # Fewer than 8 bytes received: no header can be read.
    chip = receiver(native)
    chip.poke16(0, SN_RX_WR, chip.peek16(0, SN_RX_WR) + 3)
    check(chip.recv(0)[0] == 0 and chip.stats()["malformed"] == 1, "3 bytes")
    chip.inject(good)
    check(chip.recv(0)[1] == good, "resynchronized")
    chip.close()

    # Our pointer disagrees with the chip's (as after a reset): resynchronize.
    chip = receiver(native)
    chip.poke16(0, SN_RX_WR, chip.peek16(0, SN_RX_WR) + 20000)
    check(chip.recv(0)[0] == 0 and chip.stats()["malformed"] == 1, "Sn_RX_WR out of range")
    chip.close()

    # Sn_RX_WR changing between its two bytes. The tear lands between the MSB and the LSB.
    for start, n, case in ((0x10F0, 300, "wraps below the read pointer"),
                           (0x1000, 556, "low but holds a header"),
                           (0x1000, 253, "low, under 8 bytes")):
        chip = receiver(native, open_ptr=start)
        d = bytes(range(256)) * (n // 256) + bytes(n % 256)
        chip.tear(d)
        first = chip.mark()
        got = chip.recv(0)
        reads = [f for f in chip.trace(first) if f[0] == SN_RX_WR]
        check(got[1] == d and len(reads) >= 2, f"torn Sn_RX_WR read: {case}")
        check(chip.stats()["malformed"] == 0, f"no false flush: {case}")
        chip.close()


def test_handshake(native):
    """A command waits for the previous one to be accepted (Sn_CR = 0, DS p.46), and OPEN/CLOSE
    wait for Sn_SR (DS pp.46-49), with acceptance and completion delayed independently."""
    rng = random.Random(6)
    # RECV after RECV: small datagrams leave only a few frames between two commits.
    chip = receiver(native, cmd_us=200)
    sent = [payload(rng, rng.randint(1, 20)) for _ in range(30)]
    got = []
    for d in sent:
        check(chip.inject(d) == 1, "inject")
        got.append(chip.recv(0)[1])
    check(got == sent, "small datagrams with slow command acceptance")
    chip.close()
    # CLOSE right after SEND.
    chip = sender(native, cmd_us=200)
    check(chip.send(0, b"last words") == OK, "send")
    check(chip.lib.w5500_udp_close(chip.w, 0) == OK and chip.peek(reg(0), SN_SR) == 0,
          "close waits for SEND acceptance and SOCK_CLOSED")
    chip.close()
    # OPEN and CLOSE complete long after acceptance.
    chip = Chip(native, state_us=300, cmd_us=20, open_ptr=0x1234)
    check(chip.init(tx0=8, rx0=8) == OK, "init")
    check(chip.open(0, 5004) == OK, "open waits for SOCK_UDP")
    check(chip.open(0, 5004) == OK, "reopen waits for SOCK_CLOSED, then SOCK_UDP")
    chip.dest(0, BROADCAST, 5006)
    check(chip.send(0, b"after reopen") == OK, "send")
    check([d[3] for d in chip.drain(0)] == [b"after reopen"], "pointers read after OPEN completed")
    check(chip.lib.w5500_udp_close(chip.w, 0) == OK and chip.peek(reg(0), SN_SR) == 0, "close")
    chip.close()
    # The previous SEND_OK is cleared before the next SEND, so a SEND that finishes within a
    # frame (here: a wire 1,000 times faster) is still seen.
    chip = sender(native, wire_bps=1e11)
    data = [payload(rng, TS_DATAGRAM) for _ in range(20)]
    for d in data:
        check(chip.send(0, d) == OK, "send")
    check([d[3] for d in chip.drain(0)] == data, "every completion seen")
    chip.close()


def test_publication(native):
    """Sn_RX_WR covering only part of a datagram, or moving during every read, delays delivery
    but never discards valid data."""
    rng = random.Random(7)
    for first, delay in ((8 + 500, 80.0), (4, 30.0), (8, 2000.0)):
        chip = receiver(native)
        d = payload(rng, TS_DATAGRAM)
        check(chip.inject_split(d, first, delay) == 1, "inject")
        got = b""
        for _ in range(100):
            n, got, _ = chip.recv(0)
            if n:
                break
            chip.lib.fake_advance(chip.f, 20.0)
        check(got == d and chip.stats()["malformed"] == 0,
              f"datagram published in two steps ({first} bytes, then {delay} us later)")
        chip.close()
    chip = receiver(native, churn=40)
    big = payload(rng, TS_DATAGRAM)
    check(chip.inject(big) == 1, "inject")
    out = []
    for _ in range(200):
        n, d, peer = chip.recv(0)
        if n:
            out.append(d)
        if len(out) == 41:
            break
    check(out[0] == big and out[1:] == [b"tiny"] * 40 and chip.stats()["malformed"] == 0,
          "Sn_RX_WR moving during every read")
    chip.close()


def test_settings(native):
    lib = native.lib
    s = Settings()
    lib.w5500_settings_default(ct.byref(s))
    out = ct.create_string_buffer(512)
    lib.w5500_settings_json(ct.byref(s), out, 512)
    check(out.value.decode() ==
          '"ip":"192.168.77.2","mask":"255.255.255.0","gateway":"0.0.0.0","port":5004,'
          '"dest":"255.255.255.255","dest_port":5006,"sck_khz":26667,'
          '"pins":{"sclk":9,"mosi":43,"miso":44,"cs":3}', out.value)

    def parse(line):
        return lib.w5500_settings_parse(ct.byref(s), line.encode())

    check(parse("ip 10.0.0.7/16 gw 10.0.0.1") == 1 and bytes(s.ip) == bytes([10, 0, 0, 7]) and
          bytes(s.mask) == bytes([255, 255, 0, 0]) and bytes(s.gateway) == bytes([10, 0, 0, 1]),
          "ip with gateway")
    check(parse("ip 10.0.0.8/32") == 1 and bytes(s.mask) == bytes([255] * 4) and
          bytes(s.gateway) == bytes(4), "ip alone clears the gateway")
    check(parse("dest 10.0.0.1:1234") == 1 and bytes(s.dest) == bytes([10, 0, 0, 1]) and
          s.dest_port == 1234, "unicast destination")
    check(parse("dest broadcast") == 1 and bytes(s.dest) == bytes([255] * 4) and
          s.dest_port == 1234, "broadcast keeps the port")
    check(parse("dest broadcast:5006") == 1 and s.dest_port == 5006, "broadcast with port")
    check(parse("port 6000") == 1 and s.port == 6000, "local port")
    check(parse("mhz 26.67") == 1 and s.sck_khz == 26670, "clock")
    check(parse("mhz 40") == 1 and s.sck_khz == 40000, "40 MHz")
    check(parse("pins 12 11 13 10") == 1 and (s.sclk, s.mosi, s.miso, s.cs) == (12, 11, 13, 10),
          "pins")
    before = bytes(s)
    for bad in ("", "ip", "ip 10.0.0.1", "ip 10.0.0.1/0", "ip 10.0.0.1/33", "ip 256.0.0.1/24",
                "ip 10.0.0/24", "ip 10.0.0.1.2/24", "ip 10.0.0.1/24 gw", "ip 10.0.0.1/24 gw x",
                "ip 10.0.0.1/24 extra", "dest", "dest 10.0.0.1:0", "dest 10.0.0.1:70000",
                "dest broadcastx", "dest 10.0.0.1/24", "port 0", "port 65536", "port -1",
                "mhz 9.9", "mhz 40.1", "mhz x", "pins 1 2 3", "pins 1 2 3 3", "pins 1 2 3 49",
                "defaults now", "speed 10"):
        check(parse(bad) == -1 and bytes(s) == before, f"rejects {bad!r}")
    check(parse("defaults") == 1 and s.port == 5004 and s.sck_khz == 26667, "defaults")


def main():
    with Native() as native:
        size = native.lib.fake_driver_size()
        check(size <= 300, f"driver state {size} bytes")
        test_fake(native)
        test_frame_format(native)
        test_init(native)
        test_open(native)
        test_send(native)
        test_full(native)
        test_arp_timeout(native)
        table = test_throughput(native)
        test_receive(native)
        test_malformed(native)
        test_handshake(native)
        test_publication(native)
        test_settings(native)
    print("w5500: fake checks, frames, init, open, send, full, ARP timeout, receive, malformed, "
          f"command handshake, partial publication, settings: ok (driver state {size} bytes)")
    for mhz, row in table.items():
        print(f"  {mhz:5.2f} MHz: send {row[0]:6.1f} us {row[1]:4.1f} Mb/s {row[4]:4.1f} us CPU, "
              f"receive {row[2]:6.1f} us {row[3]:4.1f} Mb/s {row[5]:4.1f} us CPU")


if __name__ == "__main__":
    main()
