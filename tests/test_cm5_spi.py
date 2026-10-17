"""Exercise CM5 framing and READY ordering without opening GPIO, SPI or serial."""
import io
import ctypes as ct
import importlib.util
import pathlib
import sys
import zlib
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'host'))
from cm5_spi import message, send
from test_ingress import Native


def pv_checks():
    path = pathlib.Path(__file__).resolve().parents[1]/'examples/pi-sender/send.py'
    spec = importlib.util.spec_from_file_location('pv_sender', path)
    pv = importlib.util.module_from_spec(spec); spec.loader.exec_module(pv)
    packet = bytes((0x47, 1, 0, 0x10))+bytes(range(184))
    assert pv.message(packet, 1)[-4:].hex() == '8d32be51'
    assert len(pv.message(packet, 1)) == 1332
    assert pv.message(packet, 0) == message(packet, 0)[3:]
    class Pipe:
        def __init__(self, data): self.data = data
        def read(self, n):
            n = min(n, 13); result, self.data = self.data[:n], self.data[n:]; return result
    assert list(pv.payloads(Pipe(packet*15))) == [packet*7, packet*7, packet]
    for bad in (packet[:-1], packet+bytes(188)):
        try: list(pv.payloads(Pipe(bad)))
        except ValueError: pass
        else: raise AssertionError('bad pipe tail accepted')
    class Clock:
        t = 0.
        def monotonic(self): return self.t
        def sleep(self, n): self.t += n
    clock = Clock(); frames = []; samples = []; released = [0.]
    class SPI:
        def xfer2(self, data):
            assert samples[-1] == clock.t and clock.t-released[0] >= 10e-6-1e-12
            frames.append(bytes(data)); clock.t += len(data)*8/20e6
            released[0] = clock.t
            return bytes(len(data))
    def ready(): samples.append(clock.t); return clock.t >= .001
    def stats():
        return dict(messages=0, attempted_messages=0, ts_packets=0, payload_bytes=0,
                    crc_chain=0, next_seq=0, ready_waits=0, ready_timeouts=0,
                    transfer_uncertain=False)
    s = stats()
    pv.send(Pipe(packet*15), SPI(), ready, s, clock=clock)
    chain = 0
    for frame in frames: chain = zlib.crc32(frame[-4:], chain)
    assert s['messages'] == s['attempted_messages'] == s['next_seq'] == 3
    assert s['ts_packets'] == 15 and s['payload_bytes'] == 15*188
    assert s['crc_chain'] == chain and s['ready_waits'] == 1
    with Native() as native:
        for frame in frames: assert native.lib.pv_check(frame, len(frame)) == 0
        for seq in (0, 1, 15, 16, 65535):
            out = ct.create_string_buffer(1316)
            native.lib.ingress_pv_pattern(out, seq)
            want = b''.join(bytes((0x47, 1, 0, 0x10 | ((7*seq+i)&15))) +
                bytes(((7*seq+i)%112+j)&255 for j in range(184)) for i in range(7))
            assert out.raw == want
    s = stats()
    try: pv.send(io.BytesIO(packet), SPI(), lambda: False, s, clock=clock)
    except TimeoutError: pass
    else: raise AssertionError('READY timeout missed')
    assert s['attempted_messages'] == 0 and s['ready_timeouts'] == 1
    class Short:
        def xfer2(self, data): return b''
    s = stats()
    try: pv.send(io.BytesIO(packet*14), Short(), lambda: True, s, clock=clock)
    except OSError: pass
    else: raise AssertionError('short write accepted')
    assert s['transfer_uncertain'] and s['attempted_messages'] == 1 and s['messages'] == 0
    print('PV raw sender: vector, fragmented stdin, READY/gap, CRC chain, timeout, no retry: ok')


def main():
    events = []
    class Clock:
        t = 0
        def monotonic(self): return self.t
        def sleep(self, seconds):
            self.t += seconds
            events.append(('sleep', seconds))
    clock = Clock()
    class SPI:
        def xfer2(self, wire):
            events.append(('spi', bytes(wire)))
            clock.t += len(wire)*8/25e6
    samples = iter([False, True, True, True])
    def ready():
        events.append(('ready', None))
        return next(samples)
    packet = b'\x47' + bytes(range(187))
    assert send(io.BytesIO(packet*15), SPI(), ready, clock=clock) == 188*15
    wires = [data for event, data in events if event == 'spi']
    assert len(wires) == 3
    for seq, wire in enumerate(wires):
        assert wire[:3] == b'\x03\0\0' and len(wire) == 1335
        m = wire[3:]
        n = 188 if seq == 2 else 1316
        assert m[:8] == b'PV\x01\x01' + seq.to_bytes(2,'little') + n.to_bytes(2,'little')
        assert m[8:12] == bytes(4) and m[12:12+n] == packet*(n//188)
        assert m[12+n:1328] == bytes(1316-n)
        assert int.from_bytes(m[-4:], 'little') == zlib.crc32(m[:-4])
    for i, (event, _) in enumerate(events):
        if event == 'spi': assert events[i-1][0] == 'ready'
        if event == 'ready': assert any(e == 'sleep' and t >= 10e-6 for e,t in events[:i])
    with Native() as native:
        for wire in wires:
            assert native.lib.pv_check(wire[3:],1332) == 0
    assert message(packet,65536) == message(packet,0)
    for bad in (b'', packet[:-1], packet*8, bytes(188)):
        try: message(bad,0)
        except ValueError: pass
        else: raise AssertionError('invalid TS accepted')
    print('CM5: mode-0 single-line framing, CRC, short final batch, READY, gap, wrap: ok')


if __name__ == '__main__':
    main()
    pv_checks()
