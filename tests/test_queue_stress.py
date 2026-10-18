"""Actual bank handoff: partial pages, wraps and notification before blocking. No board."""
import argparse
import json
import pathlib
import subprocess
from test_stream import compile_stream


def test(out):
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
    p.add_argument('output',type=pathlib.Path)
    a = p.parse_args()
    test(a.output)
