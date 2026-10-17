"""Exercise the real board script with a fake command tool. Never open hardware."""
import csv
import os
import pathlib
import shlex
import sys
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]

FAKE = r'''
import argparse, json, os, pathlib, sys, zlib
p = argparse.ArgumentParser()
p.add_argument('command'); p.add_argument('--out', type=pathlib.Path)
p.add_argument('--save', type=pathlib.Path); p.add_argument('--timeout'); p.add_argument('--reset', action='store_true')
a = p.parse_args()
with pathlib.Path('commands.jsonl').open('a') as f:
    f.write(json.dumps(vars(a), default=str) + '\n')
parts = a.command.split()
assert parts[0] in ('info', 'fecbench', 'dvbdump', 'dvbpace', 'wifimem')
if os.getenv('INJECT_FAILURES') and a.command == 'dvbpace 13 5m 30':
    sys.exit(7)
if parts[0] == 'info':
    rows = [dict(firmware='test-fw', callsign='N0CALL')]
elif parts[0] == 'fecbench':
    rows = [dict(fake_fec_ok=True)]
else:
    kind, mc = parts[0], int(parts[1])
    token = parts[2]
    sps = 8 if token in ('5m', '10m80') else 4 if token in ('10m', '20m', '20ms6') else 40
    resident = sps != 40 and (mc < 12 or sps == 4)
    rate = 80000000 if token in ('10m80', '20m', '20ms6') else 40000000
    cached = resident and mc not in (10, 11, 16, 17)
    symbols = 33282 if mc < 12 else 22194
    frame, dummy, table = (symbols // 2 + 31) // 16 * 16, 1696, 0x3fca0000
    nslots = 8 if sps == 40 else 3 if sps == 4 else 2
    short = token == '20ms6'
    used = (4096 if sps == 4 else 8192) if mc < 12 else 16384 if short else 32768
    base = table + used if mc < 12 or sps == 4 else 0x3fce0000
    slots = [base + i * frame for i in range(nslots)]
    filler = base + (nslots if mc < 12 else 2) * frame
    payload = filler + dummy
    payload_bytes = 7264 if mc == 11 else 5370
    if short:
        slots[2] = (payload + payload_bytes + 15) // 16 * 16
    elif sps == 4 and mc >= 12:
        slots[2] = 0x3fce0000
    ring_heap = 0 if mc < 12 or short else frame if sps == 4 else payload + payload_bytes - base
    r = dict(status='ESP_OK', encoder_failed=False, callsign='N0CALL', underruns=0, stall_ms=0,
             modcod=mc, symbol_rate=rate // sps, dac_rate=rate, workspace_bytes=12420,
             workspace_heap_bytes=12420, fast_bytes=18000, fast_heap_bytes=18000,
             workspace_memory='internal' if resident else 'psram',
             packed_mapper='none' if sps == 40 else 'direct' if resident else 'sparse',
             bch_cache_bytes=8192 if cached else 0, bch_variant='resident-byte' if cached else 'nibble',
             ring_slots=nslots, ring_format='raw' if sps == 40 else 'packed',
             ring_memory='psram' if sps == 40 else 'internal',
             payload_memory='psram' if sps == 40 else 'internal', pulse_symbols=6 if short else 9,
             ring_heap_bytes=ring_heap, frame_symbols=symbols, payload_bytes_per_frame=payload_bytes,
             layout=dict(table=hex(table), payload=hex(payload), filler=hex(filler), slots=list(map(hex,slots))))
    if kind == 'dvbdump':
        decim = 10 if sps == 40 else 2 if mc < 12 and sps == 8 else 1
        size = 40 * symbols * sps // decim * 4
        crc = (1435641601 if sps == 40 else
               {4: 3933043456, 11: 1552954365}[mc] if mc < 12 and sps == 8 else
               {4: 2594131925, 11: 2779934507}[mc] if mc < 12 else
               {13: 3178964763, 15: 4256019779}[mc] if short else
               {13: 1937985440, 15: 1163322913}[mc] if sps == 8 else
               {13: 474263962, 15: 1757765864}[mc])
        # A sparse zero file plus a four-byte CRC patch supplies the actual size/CRC
        # to the real checker, without storing hundreds of MB of test fixtures.
        with a.save.open('wb') as f:
            f.truncate(size)
        data = bytearray(a.save.read_bytes())
        zero = zlib.crc32(data)
        basis = {}
        for bit in range(32):
            data[-4 + bit // 8] ^= 1 << (bit % 8)
            value, mask = zlib.crc32(data) ^ zero, 1 << bit
            data[-4 + bit // 8] ^= 1 << (bit % 8)
            while value:
                top = value.bit_length() - 1
                if top not in basis:
                    basis[top] = value, mask
                    break
                v, m = basis[top]; value ^= v; mask ^= m
        value, mask = crc ^ zero, 0
        while value:
            v, m = basis[value.bit_length() - 1]; value ^= v; mask ^= m
        with a.save.open('r+b') as f:
            f.seek(size - 4); f.write(mask.to_bytes(4, 'little'))
        if os.getenv('INJECT_FAILURES') and a.command == 'dvbdump 4 40':
            crc ^= 1
        r.update(frames=40, overlap_mismatches=0, decimation=decim,
                 samples=40 * symbols * sps, crc32=crc)
        rows = [dict(binary_bytes=size, host_crc32=crc), r]
    elif kind == 'dvbpace':
        r.update(seconds=30, late_fills=0, late_switches=0, fatal_late=0, max_fill_cycles=1,
                 min_fill_slack_cycles=50000, encode_cycles_mean=1, encode_cycles_live_max=1,
                 prefill_frames=nslots, frames_played=10,
                 profile=dict(shaped_words=8, reused_words=2, fill_words=10))
        rows = [r]
    else:
        r.update(wifi_free_up=40000, wifi_min_free=39000, frames_played=0,
                 positions=0, max_fill_call_cycles=0)
        if os.getenv('INJECT_FAILURES') and mc == 17:
            r['wifi_min_free'] = 19019
        rows = [r]
with a.out.open('a') as f:
    for r in rows:
        f.write(json.dumps(dict(command=a.command, **r)) + '\n')
'''


def test_script():
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        for name in ('host', 'tests', 'bin'):
            (root / name).mkdir()
        script = (ROOT / 'host/next5-board.sh').read_text()
        interpreter = 'PY=/Library/Frameworks/Python.framework/Versions/3.13/bin/python3'
        assert interpreter in script
        (root / 'host/next5-board.sh').write_text(
            script.replace(interpreter, 'PY=' + shlex.quote(sys.executable)))
        (root / 'host/board.py').write_text(FAKE)
        (root / 'tests/test_fecbench.py').write_text(
            "import json, pathlib, sys\nassert json.loads(pathlib.Path(sys.argv[1]).read_text())['fake_fec_ok']\n")
        git = root / 'bin/git'
        git.write_text('#!/bin/sh\nprintf "test-fw\\n"\n')
        git.chmod(0o755)
        env = {**os.environ, 'PATH': str(root / 'bin') + os.pathsep + os.environ['PATH']}
        for fail in (False, True):
            calls = root / 'commands.jsonl'
            calls.unlink(missing_ok=True)
            dest = root / ('fail' if fail else 'pass')
            run = subprocess.run(['bash', str(root / 'host/next5-board.sh'), str(dest)],
                                 env={**env, **({'INJECT_FAILURES': '1'} if fail else {})},
                                 capture_output=True, text=True, timeout=120)
            assert run.returncode == int(fail), (run.stdout, run.stderr)
            import json
            commands = [json.loads(l) for l in calls.read_text().splitlines()]
            assert len(commands) == 29
            assert commands[0]['reset'] and not any(c['reset'] for c in commands[1:])
            assert sum(bool(c['save']) for c in commands) == 18
            assert [c['command'] for c in commands[-9:]] == [
                'dvbpace 13 5m 30', 'dvbpace 13 20m 30', 'dvbpace 13 20ms6 30',
                'dvbpace 4 20m 30', 'dvbpace 11 20m 30', 'dvbpace 4 30',
                'wifimem 8 10m', 'wifimem 13 20ms6', 'wifimem 17 5m']
            rows = list(csv.DictReader((dest / 'summary.tsv').open(), delimiter='\t'))
            assert len(rows) == 29
            assert sum(r['result'] == 'FAIL' for r in rows) == (3 if fail else 0), rows
            assert 'Pass criteria, fixed before running' in run.stdout
            assert f"{26 if fail else 29} passed, {3 if fail else 0} failed" in run.stdout
    print('next5 script: all 29 commands, 18 saved CRCs; command, CRC and heap failures retained; later checks still run')


if __name__ == '__main__':
    test_script()
