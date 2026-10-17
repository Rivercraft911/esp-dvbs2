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
    if command in ('dvbpace 13 10m 10','wifimem 11 26m667','spirx 1000 pv loop 9 2'):
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
        assert 'FAIL demo-pace' in summary and summary.count('FAIL') == 7, summary
        assert 'PASS gates' in summary and 'Summary (' in run.stdout
        # The optional heap sweep covers every supported MODCOD/rate, TS and GCS.
        env.update(WIFI_ALL='1', CALLS=str(folder/'all-calls'))
        run=subprocess.run(['bash',str(ROOT/'host/spi-in-board.sh'),str(folder/'all')],
                           env=env,cwd=ROOT,capture_output=True,text=True,timeout=30)
        assert run.returncode == 1
        import ast
        commands=[ast.literal_eval(line)[1] for line in (folder/'all-calls').read_text().splitlines()]
        assert commands.index('dvbpace 13 10m 10') < commands.index('spiinput ts replay')
        assert commands.count('dvbpace 13 10m 10') == 5 # TS/GCS loop and replay, no-input baseline
        assert commands.index('spiinput off') < commands.index('dvbpace 13 10m 10', commands.index('spiinput off'))
        assert '--encoder-5pct' in (folder/'all-calls').read_text()
        assert 'spirx 1000 gcs loop 0' in commands
        assert not any(token in cmd.split() for cmd in commands for token in ('10m80','20mq4','20ms6'))
        for mc in range(4,18):
            for rate in ('5m','10m','20m'):
                assert commands.count(f'wifimem {mc} {rate}') == 2
            extras=('', '26m667') if mc<12 else ()
            for rate in extras:
                assert commands.count(f'wifimem {mc} {rate}'.strip()) == 2
        env.update(CALLS=str(folder/'pv-calls'))
        run=subprocess.run(['bash',str(ROOT/'host/pv-spi-board.sh'),str(folder/'pv')],
                           env=env,cwd=ROOT,capture_output=True,text=True,timeout=20)
        assert run.returncode == 1, run.stdout+run.stderr
        calls=(folder/'pv-calls').read_text()
        commands=[ast.literal_eval(line)[1] for line in calls.splitlines()]
        assert commands[:3] == ['info', 'spipins devkit', 'spirx 3000 pv loop 9']
        assert 'dvbpace 13 10m 30' in commands and 'spirx 1000 gcs loop 46.6' in commands
        assert commands[-1] == 'spiinput off' and 'reg-8psk.iq' in calls and "'check'" in calls
        decode=[ast.literal_eval(line) for line in calls.splitlines()
                if "'decode'" in line]
        assert len(decode) == 1
        args=decode[0]
        # dvbdump 13 5m, decimation 1: 40 MS/s / 5 Msym/s = 8 samples/symbol.
        assert '--pv' in args and args[args.index('--modcod')+1] == '13'
        assert args[args.index('--sps')+1] == '8' and args[args.index('--rate')+1] == '40000000'
        summary=(folder/'pv/summary.txt').read_text()
        assert summary.count('FAIL') == 1 and 'FAIL pv-fault-2' in summary and 'PASS gates' in summary
        assert run.stdout.index('PASS requires') < run.stdout.index('Summary')
    print('SPI shells: keep going after failures, final summary, PV/quad/regression checks and cleanup: ok')


if __name__ == '__main__': main()
