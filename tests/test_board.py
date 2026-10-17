"""Retain partial results and device diagnostics on timeout, with a fake USB link."""
import itertools
import pathlib
import sys
import types
from unittest.mock import patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'host'))
with patch.dict(sys.modules, {'serial': types.ModuleType('serial'),
                              'serial.tools': types.SimpleNamespace(list_ports=None)}):
    import board


class Link:
    def __init__(self, chunks):
        self.chunks = iter(chunks)
        self.closed = False

    def reset_input_buffer(self): pass
    def write(self, data): assert data == b'wifimem 5 26m667\n'
    def read(self, n): return next(self.chunks, b'')
    def close(self): self.closed = True


def main():
    for exists, ports, explicit, expected in (
        (True, ['cu.usbmodem1101'], board.DEFAULT_PORT, board.DEFAULT_PORT),
        (False, ['cu.usbmodem1101'], board.DEFAULT_PORT, '/dev/cu.usbmodem1101'),
        (False, [], board.DEFAULT_PORT, board.DEFAULT_PORT),
        (False, ['cu.usbmodem1101','cu.usbmodem5981'], board.DEFAULT_PORT, board.DEFAULT_PORT),
        (False, ['cu.usbmodem1101'], '/dev/custom', '/dev/custom'),
    ):
        with patch.object(pathlib.Path, 'exists', return_value=exists), \
             patch.object(pathlib.Path, 'glob', return_value=[pathlib.Path('/dev')/p for p in ports]):
            assert board.resolve_port(explicit) == expected
    record = b'@{"cmd":"wifimem","wifi_min_free":19999}\n'
    for complete in (False, True):
        link = Link([b'Wi-Fi diagnostic\n', record,
                     b'@{"done":true}\n' if complete else b'unfinished diagnostic'])
        with patch.object(board, 'open_port', return_value=link), \
             patch.object(board.time, 'monotonic', side_effect=itertools.count()):
            if complete:
                assert board.run('fake', 'wifimem 5 26m667', 10) == [
                    dict(cmd='wifimem', wifi_min_free=19999)]
            else:
                try: board.run('fake', 'wifimem 5 26m667', 10)
                except TimeoutError as e:
                    assert all(s in str(e) for s in ('19999', 'Wi-Fi diagnostic', 'unfinished diagnostic'))
                else: raise AssertionError('incomplete command passed')
        assert link.closed
    print('board reader: completed results, timeout records/text and closed fake link: ok')


if __name__ == '__main__': main()
