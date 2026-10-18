"""8PSK admission, producer/consumer ownership, bank seams and queue wrap. No board."""
import argparse
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import numpy as np
from test_stream import compile_stream

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT/'host'),str(ROOT/'tests/reference')]
import dvbs2
import psk8_26m667 as lofty
import waveforms


def check_admission(out):
    out.mkdir(parents=True,exist_ok=True)
    exe = compile_stream(out,source='psk8_admission.c')
    for mc in (13,15,17):
        for slow in (0,1):
            for full in (False,True):
                args = [str(exe),str(mc),str(slow)]+(['full'] if full else [])
                run = subprocess.run(args,capture_output=True,text=True,timeout=30)
                (out/f'{mc}-{slow}-{int(full)}.log').write_text(run.stdout+run.stderr)
                assert run.returncode == 0,(mc,slow,full,run.stdout[-2000:],run.stderr)
                row, = [json.loads(l[1:]) for l in run.stdout.splitlines() if l.startswith('@{')]
                assert row['data'] >= 1000 and (full or row['streamed'] > 0)
                assert row['threshold'] == 10
                assert row['steady'] > 0 if slow else row['steady'] == 0
                print(row,flush=True)

def check_producer(out):
    for full in (False,True):
        where = out/('full' if full else 'normal')
        where.mkdir(parents=True,exist_ok=True)
        exe = compile_stream(where,source='psk8_producer.c',defines=['QUEUE_TEST_PAGES=44'] if full else [])
        for mc in (13,15,17):
            for dump in (False,True):
                args = [str(exe),str(mc)]+(['dump'] if dump else [])
                run = subprocess.run(args,capture_output=True,text=True,timeout=15)
                (where/f'{mc}-{int(dump)}.log').write_text(run.stdout+run.stderr)
                assert run.returncode == 0,(mc,full,dump,run.stdout,run.stderr)
                rows = [json.loads(l[1:]) for l in run.stdout.splitlines() if l.startswith('@{')]
                assert rows[0]['fec_ahead'] == 1
                assert not full or rows[0]['bb_while_full'] == rows[0]['ldpc_while_full'] == 1
                assert rows[1] == dict(test='publication_clock',gap=1000,active_pages=10,wait_pages=26)
                print(mc,'dump' if dump else 'pace','full queue' if full else 'normal queue',
                      'FEC precedes wait; cold accounting leaves 10/26',flush=True)

def check_handoff(out):
    out.mkdir(parents=True,exist_ok=True)
    exe = compile_stream(out,source='psk8_handoff.c')
    for mc in (13,15,17):
        rate, modulation = waveforms.mode(mc)
        bits = np.unpackbits(np.frombuffer(waveforms.payload(mc,1,'N0CALL'),np.uint8))
        fec = dvbs2.fecframe(dvbs2.bbframe(bits,rate),rate)
        phases = dvbs2.plframe_phases(fec,rate,modulation=modulation)
        for before in (None,1,2):
            name = f'{mc}-{before}'
            iq = out/f'{name}.iq'
            env = dict(os.environ)
            if before is not None: env['FORCE_DUMMY_FRAME'] = str(before)
            run = subprocess.run([str(exe),str(mc),str(iq)],env=env,capture_output=True,text=True,timeout=60)
            (out/f'{name}.log').write_text(run.stdout+run.stderr)
            assert run.returncode == 0,(name,run.stdout,run.stderr)
            if before is None:
                symbols = np.tile(phases,6)
            else:
                symbols = np.concatenate((np.tile(phases,before),np.tile(dvbs2.dummy_phases(),2),
                                          np.tile(phases,6-before)))
                rows = [json.loads(l[1:]) for l in run.stdout.splitlines() if l.startswith('@{')]
                late = next(r for r in rows if r.get('test') == 'transition' and r.get('first') == (417660772 if before == 1 else 417723732))
                assert late['dummy_before'] == 0 and late['dummy_after'] == (2 if before == 1 else 1),late
                assert late['singles'] <= 5 and late['switches'] == 1,late
                switches = [(r['relative'],r['kind']) for r in rows if r.get('test') == 'source']
                assert switches == [(before*66582,'dummy'),(before*66582+19980,'data')],switches
            expected = lofty.iq(lofty.shape(symbols))
            actual = np.fromfile(iq,dtype='<i2').reshape(-1,2)
            np.testing.assert_array_equal(actual,np.column_stack((expected.real,expected.imag)))
            rows = [json.loads(l[1:]) for l in run.stdout.splitlines() if l.startswith('@{')]
            bank, = [r for r in rows if r.get('test') == 'round17_bank']
            record, = [r for r in rows if 'dummy_startup_frame' in r]
            assert record['dummy_startup_frame'] == (before if before is not None else -1)
            assert bank['singles'] <= 5, bank
            assert bank['dummy_samples'] > 0 if before == 2 else bank['dummy_samples'] == 0
            print(name,'ID, EOF, dummy pair and return: every word exact; all slices reach bulk',flush=True)

def check_stress(out):
    out.mkdir(parents=True,exist_ok=True)
    exe = compile_stream(out,source='queue_stress.c')
    hashes = {}
    for mc in (13,15,17):
        for seed in (7,2026):
            name = f'{mc}-{seed}'
            r = subprocess.run([str(exe),str(mc),str(seed)],capture_output=True,text=True,timeout=30)
            (out/(name+'.log')).write_text(r.stdout+r.stderr)
            assert r.returncode == 0,(name,r.stdout[-1500:],r.stderr)
            rows = [json.loads(l[1:]) for l in r.stdout.splitlines() if l.startswith('@{')]
            result = next(row for row in rows if row.get('test') == 'queue_stress')
            assert result['pages'] == 4096 and result['wraps'] >= 75
            assert result['early_notifications'] == 1
            assert result['data_hash'] == hashes.setdefault(mc,result['data_hash'])
            print(name,'4096 pages intact; late producer replaced by dummies; data intact',flush=True)

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('output',nargs='?',type=pathlib.Path)
    a = p.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        out = (a.output or pathlib.Path(tmp)).resolve()
        for name, fn in (("admission",check_admission),("producer",check_producer),
                         ("handoff",check_handoff),("stress",check_stress)):
            fn(out/name)
