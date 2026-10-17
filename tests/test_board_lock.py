"""Serial ownership without opening a device. Real locks, mocked lsof/USB inventory."""
import importlib.util
import os
import pathlib
import subprocess
import sys
import tempfile
import types
from unittest.mock import patch

ROOT = pathlib.Path(__file__).resolve().parents[1]
serial = types.ModuleType('serial')
serial.tools = types.ModuleType('serial.tools')
serial.tools.list_ports = types.SimpleNamespace(comports=lambda: [])
spec = importlib.util.spec_from_file_location('board', ROOT/'host/board.py')
board = importlib.util.module_from_spec(spec)
with patch.dict(sys.modules, {'serial': serial, 'serial.tools': serial.tools}):
    spec.loader.exec_module(board)

with tempfile.TemporaryDirectory() as tmp:
    port = str(pathlib.Path(tmp)/'device')
    pathlib.Path(port).touch()
    clear = subprocess.CompletedProcess([], 1, '', '')
    busy = subprocess.CompletedProcess([], 0, '123\n456\n', '')
    with patch.object(board.subprocess, 'run', return_value=busy):
        try: board.reserve_port(port)
        except RuntimeError as e: assert 'busy' in str(e) and '123, 456' in str(e)
        else: raise AssertionError('busy port admitted')
    with patch.object(board.subprocess, 'run', return_value=clear):
        lock = board.reserve_port(port)
        try:
            try: board.reserve_port(port)
            except BlockingIOError: pass
            else: raise AssertionError('second coordinator admitted')
            with patch.dict(os.environ, ESP_DVBS2_LOCK_FD=str(lock.fileno())):
                with board.reserve_port(port): pass
            # Failed ownership checks release their file descriptions.
        finally: lock.close()
        with board.reserve_port(port): pass
    bridge = str(pathlib.Path(tmp)/'bridge')
    pathlib.Path(bridge).touch()
    with patch.object(board, 'bridge_port', return_value=bridge), \
         patch.object(board.subprocess, 'run', return_value=busy) as probe:
        try: board.reserve_port(port, reset=True)
        except RuntimeError: pass
        else: raise AssertionError('busy reset bridge admitted')
        assert bridge in probe.call_args.args[0]
    # An ownership-check error must fail closed, too.
    with patch.object(board.subprocess, 'run', side_effect=FileNotFoundError('lsof')):
        try: board.reserve_port(port)
        except FileNotFoundError: pass
        else: raise AssertionError('missing lsof admitted')
print('board ownership: busy users/bridge rejected, lock exclusion/inheritance and cleanup pass; no serial opens')
