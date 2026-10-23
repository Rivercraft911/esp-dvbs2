"""board.py --monitor: status records go out as monitor datagrams and stay out of the results.
No serial port: a scripted link stands in for the board."""
import json
import pathlib
import socket
import struct
import sys
import time
import types

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'host'))
try:
    import serial  # noqa: F401
except ImportError:  # board.py needs pyserial only to open a real port
    sys.modules['serial'] = types.SimpleNamespace(Serial=None)
    sys.modules['serial.tools'] = types.SimpleNamespace(list_ports=None)
import board


class Link:
    """The board's side of one command, in chunks that split lines anywhere."""
    def __init__(self, chunks):
        self.chunks, self.written = list(chunks), b''

    def reset_input_buffer(self):
        pass

    def write(self, data):
        self.written += data

    def read(self, n):
        return self.chunks.pop(0) if self.chunks else b''

    def close(self):
        pass


def main():
    status = [dict(cmd='status', run='dvb', elapsed_s=1.0 + k, freq_mhz=2420, frames=270 * k,
                   dummies=0, encoder_spare_pct=None) for k in range(3)]
    result = dict(cmd='dvb', modcod=14, frames_played=810, status='ESP_OK')
    text = b''.join(b'@' + json.dumps(r).encode() + b'\n' for r in status)
    text = b'ESP-ROM text\n' + text + b'@' + json.dumps(result).encode() + b'\n@{"cmd":"dvb","done":true}\n'
    chunks = [text[i:i + 37] for i in range(0, len(text), 37)]

    listener = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    listener.bind(('127.0.0.1', 0))
    listener.settimeout(2)
    monitor = board.Monitor('127.0.0.1:%d' % listener.getsockname()[1])
    monitor.seq = 0xFFFE                         # the 16-bit sequence wraps in this run
    board.open_port = lambda port, reset: Link(chunks)
    before = time.monotonic_ns() // 1000
    records = board.run('scripted', 'dvb 2420 14 5m 3 --short', timeout=5, live=monitor)
    after = time.monotonic_ns() // 1000

    # The results and their printing are what they were before status lines existed.
    assert records == [result], records
    sent = [json.loads(listener.recv(65536)[12:]) for _ in range(4)]
    assert sent == status + [result], sent
    listener.settimeout(0.2)
    try:
        listener.recv(65536)
        raise AssertionError('the done record must not be forwarded')
    except socket.timeout:
        pass

    # Header: b'PVMN', version 1, type 4, sequence, low 32 bits of host monotonic microseconds.
    listener.settimeout(2)
    monitor.seq = 0xFFFF
    for record in status[:2]:
        monitor(record)
    for seq in (0xFFFF, 0):
        data = listener.recv(65536)
        magic, version, kind, got, stamp = struct.unpack_from('<4sBBHI', data)
        assert (magic, version, kind, got) == (b'PVMN', 1, 4, seq)
        assert (stamp - (before & 0xFFFFFFFF)) & 0xFFFFFFFF <= (after - before) + 10_000_000
        assert json.loads(data[12:].decode('utf-8'))['cmd'] == 'status'

    # A send that fails is counted, never raised: the board's run goes on regardless.
    monitor.socket.close()
    monitor(result)
    assert monitor.failed == 1
    for bad in ('1235', 'localhost', 'localhost:0', 'localhost:65536', ':1235', 'host:port'):
        try:
            board.Monitor(bad)
            raise AssertionError(bad)
        except ValueError:
            pass
    assert board.Monitor('[::1]:1235').target == ('::1', 1235)
    print('board --monitor: status records forwarded as PVMN type 4 and kept out of the results; '
          'header, sequence wrap and failed sends checked')


if __name__ == '__main__':
    main()
