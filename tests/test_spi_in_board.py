"""Run the real coordinator shell with fake board/decode tools. No hardware access."""
import os
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
FAKE = r'''#!/usr/bin/env python3
import os,pathlib,sys
args=sys.argv[1:]
with open(os.environ['CALLS'],'a') as f: f.write(repr(args)+'\n')
if args[0]=='host/board.py':
    command=args[1]
    assert command.split()[0] in ('info','spipins','spiinput','spirx','dvbdump','dvbpace','wifimem')
    assert '--reset' not in args
    if command in ('dvbpace 13 10m 10','wifimem 11 26m667'):
        raise SystemExit(7)
elif args[0]=='host/spi_input.py':
    assert args[1] in ('decode','check')
else: raise AssertionError(args)
'''


def main():
    with tempfile.TemporaryDirectory() as tmp:
        folder=pathlib.Path(tmp)
        fake=folder/'python'; fake.write_text(FAKE); fake.chmod(0o755)
        env=dict(os.environ, PY=str(fake), CALLS=str(folder/'calls'), SECONDS_PER_RUN='10')
        run=subprocess.run(['bash',str(ROOT/'host/spi-in-board.sh'),str(folder/'out')],
                           env=env,cwd=ROOT,capture_output=True,text=True,timeout=20)
        assert run.returncode == 1, run.stdout+run.stderr
        calls=(folder/'calls').read_text()
        assert 'reg-qpsk.iq' in calls and 'reg-8psk.iq' in calls and "'check'" in calls
        assert calls.rstrip().endswith("'--out', '"+str(folder/"out/runs.jsonl")+"']"), calls
        assert calls.splitlines()[-1].startswith("['host/board.py', 'spiinput off'")
        summary=(folder/'out/summary.txt').read_text()
        assert 'FAIL demo-pace' in summary and summary.count('FAIL') == 5, summary
        assert 'PASS gates' in summary and 'Summary (' in run.stdout
        # The optional heap sweep covers every supported MODCOD/rate, TS and GCS.
        env.update(WIFI_ALL='1', CALLS=str(folder/'all-calls'))
        run=subprocess.run(['bash',str(ROOT/'host/spi-in-board.sh'),str(folder/'all')],
                           env=env,cwd=ROOT,capture_output=True,text=True,timeout=30)
        assert run.returncode == 1
        import ast
        commands=[ast.literal_eval(line)[1] for line in (folder/'all-calls').read_text().splitlines()]
        assert commands.index('dvbpace 13 10m 10') < commands.index('spiinput ts replay')
        assert commands.count('dvbpace 13 10m 10') == 3 # wire loop, TS replay, GCS replay
        assert 'spirx 1000 gcs loop 0' in commands
        assert not any(token in cmd.split() for cmd in commands for token in ('10m80','20mq4','20ms6'))
        for mc in range(4,18):
            for rate in ('5m','10m','20m'):
                assert commands.count(f'wifimem {mc} {rate}') == 2
            extras=('', '26m667') if mc<12 else ()
            for rate in extras:
                assert commands.count(f'wifimem {mc} {rate}'.strip()) == 2
    print('SPI shell: demo first, slim runs, continues after failures, final summary and cleanup: ok')


if __name__ == '__main__': main()
