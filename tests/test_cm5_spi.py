"""Exercise CM5 framing and READY ordering without opening GPIO, SPI or serial."""
import io
import pathlib
import sys
import zlib
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'host'))
from cm5_spi import message, send
from test_ingress import Native


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


if __name__ == '__main__': main()
