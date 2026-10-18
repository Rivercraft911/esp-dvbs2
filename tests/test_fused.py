"""Bit-exact fused tables, split queues and allocation fallback. No hardware."""
import json
import subprocess
import tempfile
from pathlib import Path
from test_stream import compile_stream

ROOT = Path(__file__).resolve().parents[1]

with tempfile.TemporaryDirectory() as folder:
    out = Path(folder)
    exe = compile_stream(out, source='psk8_fused.c')
    run = subprocess.run([str(exe)], text=True, capture_output=True, timeout=90)
    assert run.returncode == 0, (run.stdout[-8000:], run.stderr)
    rows = [json.loads(line[1:]) for line in run.stdout.splitlines() if line.startswith('@{')]
    fused = [r for r in rows if r.get('test') == 'fused']
    assert len(fused) == 36
    assert sum(r['selected'] for r in fused) == 18
    assert sum(r['split'] for r in fused) == 6
    assert all(r['queue_pages'] == 54 and not r['queue_empty'] for r in fused)
    keyed = [r for r in rows if r.get('cmd') == 'dvb']
    paces = [r for r in rows if r.get('cmd') == 'dvbpace']
    assert len(keyed) == len(paces) == 12
    for group in (keyed, paces):
        for mc in (13, 15, 17):
            subset = [r for r in group if r['modcod'] == mc]
            assert [(r['phy_solo'], r['fused_tails']) for r in subset] == [
                (False, False), (False, True), (True, False), (True, True)]
            assert all(r['status'] == 'ESP_OK' and r['queue_pages'] == 54 for r in subset)
    assert all(r['prepared_fixture'] and r['payload_source'] == 'psram-frame' for r in keyed)
    print(run.stdout.splitlines()[-1])
