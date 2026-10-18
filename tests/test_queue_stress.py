"""Stress the actual page publisher/consumer; replay a FreeRTOS peer-task pause. No board."""
import argparse
import json
import pathlib
import subprocess
from test_stream import compile_stream


def test(out):
    out.mkdir(parents=True,exist_ok=True)
    exe = compile_stream(out,source='queue_stress.c',defines=('QUEUE_STRESS','STREAM_PROFILE_HOST'))
    hashes = {}
    for mc in (13,15,17):
        for phase in (2,3,4):
            # Priority-1 peers must not delay publication. A higher-priority
            # stall must still fail honestly and preserve both queue views.
            for peer,fault in ((1,0),(5,1)):
                name = f'{mc}-{phase}-{peer}'
                r = subprocess.run([str(exe),str(mc),str(phase),str(peer),str(fault)],
                                   capture_output=True,text=True,timeout=30)
                (out/(name+'.log')).write_text(r.stdout+r.stderr)
                assert r.returncode == 0,(name,r.stdout[-1500:],r.stderr)
                rows = [json.loads(l[1:]) for l in r.stdout.splitlines() if l.startswith('@{')]
                result = next(row for row in rows if row.get('test') == 'queue_stress')
                if not fault:
                    assert result['pages'] == 4096 and result['wraps'] >= 75
                    assert result['waveform_hash'] == hashes.setdefault(mc,result['waveform_hash'])
                if fault:
                    snapshot = next(row for row in rows if 'consumer_ready' in row)
                    assert snapshot['consumer_ready'] == 0
                    if phase == 3: assert snapshot['producer_ready'] > 0
                print(name,'fault reproduced' if fault else '4096 pages intact',flush=True)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('output',type=pathlib.Path)
    a = p.parse_args()
    test(a.output)
