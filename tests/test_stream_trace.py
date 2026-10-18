"""On-chip trace framing and real threaded publication/release matching. No board."""
import argparse
import importlib.util
import json
import os
import pathlib
import subprocess
from test_stream import compile_stream, ROOT


def check(out):
    out.mkdir(parents=True,exist_ok=True)
    exe = compile_stream(out,source='lofty_trace.c')
    unit = out/'trace_unit'
    subprocess.run(['cc','-O2','-std=c11','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(out),'-I'+str(ROOT/'firmware/main'),
                    str(ROOT/'tests/stream_trace.c'),'-o',str(unit)],check=True)
    subprocess.run([str(unit)],check=True)
    spec = importlib.util.spec_from_file_location('coordinator',ROOT/'host/jitter.py')
    coordinator = importlib.util.module_from_spec(spec); spec.loader.exec_module(coordinator)
    for mc in (13,15,17):
        run = subprocess.run([str(exe),str(mc)],env=dict(os.environ,FORCE_DUMMY_FRAME='68'),
                             capture_output=True,text=True,timeout=60)
        (out/f'{mc}.log').write_text(run.stdout+run.stderr)
        assert run.returncode == 0,run.stderr
        rows = [json.loads(l[1:]) for l in run.stdout.splitlines() if l.startswith('@{')]
        off, on = [r for r in rows if r.get('test') == 'trace_lifecycle']
        assert 'stream_trace' not in off and not off['traced'] and on['traced']
        assert off['crc'] == on['crc']
        summary = coordinator.trace_summary(on)
        assert summary['frames'] == 64 and summary['map_waits'] > 0
        assert summary['dummies_before_data'] == summary['max_dummies_before_data'] == 2
        t = on['stream_trace']
        assert t['single_page_batches'] and t['storage'] == 'psram'
        print(mc,'64 threaded frames, all 2816 releases matched, forced dummies counted, waveform unchanged')
        t['complete'] = False
        try: coordinator.trace_summary(on)
        except AssertionError as e: assert 'incomplete' in str(e)
        else: raise AssertionError('incomplete trace accepted')
    plan = subprocess.check_output(['python3',str(ROOT/'host/jitter.py'),'/unused','--trace','--plan'],text=True)
    assert plan.splitlines() == ['info (reset once)',
                                'dvbpace 15 26m667 10 128 0 observe trace',
                                'dvbpace 17 26m667 10 128 0 observe trace']
    print('trace coordinator: exactly two observed 10-second commands; incomplete capture rejected')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('output',type=pathlib.Path)
    check(p.parse_args().output.resolve())
