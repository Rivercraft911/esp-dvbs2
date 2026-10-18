"""Streaming admission, fastest page consumption and bounded production. No board."""
import argparse
import json
import pathlib
import subprocess
from test_stream import compile_stream


def test(out):
    out.mkdir(parents=True,exist_ok=True)
    exe = compile_stream(out,source='lofty_admission.c')
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



if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('output',type=pathlib.Path)
    test(p.parse_args().output.resolve())
