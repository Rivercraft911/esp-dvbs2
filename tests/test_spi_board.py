"""Run the real coordinator shell with fake board/decode tools. No hardware access."""
import os
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
FAKE = r'''#!/usr/bin/env python3
import os,sys
args=sys.argv[1:]
with open(os.environ['CALLS'],'a') as f: f.write(repr(args)+'\n')
if args[0]=='host/board.py':
    command=args[1]
    assert command.split()[0] in ('info','spipins','spiinput','spirx','dvbdump','dvbpace')
    assert '--reset' not in args
    if command == 'spirx 1000 on loop 9 2':
        raise SystemExit(7)
elif args[0]=='host/spi_input.py':
    assert args[1] in ('decode','check')
else: raise AssertionError(args)
'''


def main():
    with tempfile.TemporaryDirectory() as tmp:
        folder=pathlib.Path(tmp)
        fake=folder/'python'; fake.write_text(FAKE); fake.chmod(0o755)
        env=dict(os.environ, PY=str(fake), CALLS=str(folder/'calls'))
        run=subprocess.run(['bash',str(ROOT/'host/spi-board.sh'),str(folder/'check')],
                           env=env,cwd=ROOT,capture_output=True,text=True,timeout=20)
        assert run.returncode == 1, run.stdout+run.stderr
        calls=(folder/'calls').read_text()
        import ast
        commands=[ast.literal_eval(line)[1] for line in calls.splitlines()]
        assert commands[:3] == ['info', 'spipins devkit', 'spirx 3000 on loop 9']
        assert 'dvbpace 13 10m 30' in commands and 'spirx 1000 quad gcs loop 46.6' in commands
        assert 'spirx 1000 on gcs loop 9' in commands and 'spiinput on gcs loop 12' in commands
        pace = commands.index('dvbpace 13 5m 10')
        assert commands[pace-1] == 'spiinput on gcs loop 6'
        assert commands[-1] == 'spiinput off' and 'reg-8psk.iq' in calls and "'check'" in calls
        decode=[ast.literal_eval(line) for line in calls.splitlines()
                if "'decode'" in line]
        assert len(decode) == 2
        args=decode[0]
        # dvbdump 13 5m, decimation 1: 40 MS/s / 5 Msym/s = 8 samples/symbol.
        assert args[args.index('--frames')+1] == '16'
        assert '--pv' in args and args[args.index('--modcod')+1] == '13'
        assert args[args.index('--sps')+1] == '8' and args[args.index('--rate')+1] == '40000000'
        assert decode[1][decode[1].index('--kind')+1] == 'gcs' and '--pv' not in decode[1]
        summary=(folder/'check/summary.txt').read_text()
        assert summary.count('FAIL') == 1 and 'FAIL raw-fault-2' in summary and 'PASS gates' in summary
        assert run.stdout.index('PASS requires') < run.stdout.index('Summary')
    print('SPI shells: keep going after failures, final summary, raw TS/GCS, quad and regression checks and cleanup: ok')


if __name__ == '__main__': main()
