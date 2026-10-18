"""FEC before page waits, independent FEC bytes and cold admission accounting."""
import argparse
import json
import pathlib
import subprocess
from test_stream import compile_stream


def test(out):
    for full in (False,True):
        where = out/('full' if full else 'normal')
        where.mkdir(parents=True,exist_ok=True)
        exe = compile_stream(where,source='lofty_producer.c',defines=['QUEUE_TEST_PAGES=44'] if full else [])
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



if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('output',type=pathlib.Path)
    test(p.parse_args().output.resolve())
