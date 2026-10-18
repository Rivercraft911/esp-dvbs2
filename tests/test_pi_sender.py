"""Reference sender framing and READY ordering without GPIO, SPI or serial."""
import io
import ctypes as ct
import importlib.util
import pathlib
import zlib
from test_ingress import Native


def main():
    path = pathlib.Path(__file__).resolve().parents[1]/'examples/pi-sender/send.py'
    spec = importlib.util.spec_from_file_location('pv_sender', path)
    pv = importlib.util.module_from_spec(spec); spec.loader.exec_module(pv)
    packet = bytes((0x47, 1, 0, 0x10))+bytes(range(184))
    assert pv.message(packet, 1)[-4:].hex() == '8d32be51'
    assert len(pv.message(packet, 1)) == 1332
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
        for kind, payload in ((0, b''), (2, b'x'), (2, bytes(range(256))*5+b'end')):
            frame = pv.message(payload, 65535, kind)
            assert len(frame) == 1332 and frame[3] == kind
            assert frame[12:12+len(payload)] == payload
            assert frame[12+len(payload):1328] == bytes(1316-len(payload))
            assert native.lib.pv_check(frame, len(frame)) == 0
            assert pv.message(payload, 65536, kind) == pv.message(payload, 0, kind)
        for payload, kind in ((b'', 1), (b'', 2), (b'x', 0), (b'x', 3), (bytes(1317), 2)):
            try: pv.message(payload, 0, kind)
            except ValueError: pass
            else: raise AssertionError('invalid PV type/length accepted')
        for seq in (0, 1, 15, 16, 65535):
            out = ct.create_string_buffer(1316)
            native.lib.ingress_pv_pattern(out, seq)
            want = b''.join(bytes((0x47, 1, 0, 0x10 | ((7*seq+i)&15))) +
                bytes(((7*seq+i)%112+j)&255 for j in range(184)) for i in range(7))
            assert out.raw == want
    frames.clear(); s = stats()
    generic = bytes(range(256))*6
    pv.send(Pipe(generic), SPI(), ready, s, kind=2, clock=clock)
    assert len(frames) == 2 and all(frame[3] == 2 for frame in frames)
    assert frames[0][12:1328]+frames[1][12:12+220] == generic
    assert s['payload_bytes'] == len(generic) and s['ts_packets'] == s['crc_chain'] == 0
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


if __name__ == '__main__':
    main()
