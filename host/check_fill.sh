#!/bin/sh
# Coordinator handoff for an experiment that fails admission. --plan opens no devices.
cd "$(dirname "$0")/.." || exit 1
exec "${PYTHON:-python3}" - "$@" <<'PY'
import argparse
import datetime
import json
import os
import pathlib
import subprocess
import sys

p = argparse.ArgumentParser(description='Offline fill gates; --probe adds only the radio-off kernel benchmark.')
p.add_argument('out',nargs='?',type=pathlib.Path)
p.add_argument('--plan',action='store_true')
p.add_argument('--probe',action='store_true',help='coordinator only: run membench moon after build/flash')
p.add_argument('--qemu-log',type=pathlib.Path)
a = p.parse_args()
golden = json.loads(pathlib.Path('tests/data/moon_waveform_crcs.json').read_text())
print('Fixed before board commands: peak 128, N0CALL, 16 frames, little-endian I16/Q16.')
print('All 57 retained CRCs unchanged; every new C dump equals Python word for word.')
for name,crc in golden.items(): print('  native dump',name,'CRC32',crc)
print('Receiver input adds 4096 zero samples after the dump, outside the pinned IQ CRC, to drain lookahead.')
print('LeanDVB: >=14 contiguous QPSK frames or >=12 8PSK; all payload and FEC error/correction counts zero.')
print('Native C: 324 block cases, 15552 guarded slices, exact tables/hashes; --qemu-log checks PIE too. All allocation failures clean up.')
print('Mask: positive Annex A margins with ideal DAC hold, resampling aliases and exact table-rounding PSD.')
print('Admission: complete worst bank <=40137 cycles of 47220; Wi-Fi-up free and low-water heap >=20000 B.')
print('Probe screen: median full-bank kernel <=34137 cycles, reserving 6000 for stream work. Necessary, not sufficient.')
print('New modes have no dvbpace/dvbdump/wifimem integration. Admission FAIL until actual 16-frame board decodes, 30 s paces and memory gates exist.')
print('Existing-mode coordinator regressions remain host/check.sh; its original measured heap floors are unchanged.')
print('Keep going after failures. Never flashes, starts Wi-Fi, plays samples or keys RF.',flush=True)
if a.plan: sys.exit(0)
out = a.out or pathlib.Path('notes',datetime.datetime.now().strftime('fill-%Y%m%d-%H%M%S'))
out.mkdir(parents=True,exist_ok=False)
summary = []
def step(label,cmd):
    try:
        with (out/(label+'.log')).open('w') as f:
            result = subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT,timeout=240)
        assert result.returncode == 0,f'exit {result.returncode}'
        status,detail = 'PASS',''
    except Exception as e: status,detail = 'FAIL',str(e)
    summary.append((status,label,detail))
    print(status+': '+label+(' '+detail if detail else ''),flush=True)
python = sys.executable
test = [python,'tests/test_moon_fill.py']
if a.qemu_log: test.append(str(a.qemu_log))
step('kernels',test)
step('legacy-crcs',[python,'tests/test_stream.py'])
rx = os.getenv('RX','/Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb')
step('native-decodes',[python,'host/moon_fill.py','--receiver',rx,'--output',str(out/'dumps')])
if a.probe:
    step('probe',[python,'host/board.py','membench moon','--reset','--timeout','120','--out',str(out/'runs.jsonl')])
    step('probe-check',[python,'host/moon_fill.py','--membench',str(out/'runs.jsonl')])
    try:
        rows = [json.loads(l) for l in (out/'probe-check.log').read_text().splitlines()]
        assert len(rows) == 4 and all(r['probe_target_met'] for r in rows)
        summary.append(('PASS','probe-screen',''))
    except Exception: summary.append(('FAIL','probe-screen','kernel exceeds 34137 cycles or records incomplete'))
summary.append(('FAIL','admission','live stream, split timing and Wi-Fi heap checks are absent'))
with (out/'summary.tsv').open('w') as f:
    for row in summary: f.write('\t'.join(row)+'\n')
print('FAIL: admission; candidates remain experiments',flush=True)
sys.exit(1)
PY
