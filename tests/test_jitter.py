"""Nested ISR attribution, exact teardown, counter wraps and histogram boundaries. No board."""
import json
import pathlib
import subprocess
import tempfile
import importlib.util

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory() as tmp:
        exe = pathlib.Path(tmp)/'jitter'
        subprocess.run(['cc','-O2','-std=c11','-Wall','-Wextra','-Werror',
                        str(ROOT/'tests/jitter.c'),'-o',str(exe)],check=True)
        r = json.loads(subprocess.check_output([str(exe)],text=True))['jitter']
        assert sum(b['calls'] for b in r['banks_by_irq_count']) == 10
        assert [sum(b['histogram'][i] for b in r['banks_by_irq_count']) for i in range(6)] == [1,2,2,2,2,1]
        assert r['cores'][0][0]['cycles'] == r['cores'][0][1]['cycles'] == 20
        assert r['cores'][1][0]['calls'] == 20
        assert r['worst_bank']['irq_cycles'] == 400
    print('jitter observer: nested handlers, original arguments, teardown, wraps and bin edges: ok')
    spec = importlib.util.spec_from_file_location('jitter_check',ROOT/'host/jitter.py')
    check = importlib.util.module_from_spec(spec); spec.loader.exec_module(check)
    sent = []

    def send(command):
        sent.append(command)
        if command == 'info': return [dict(firmware='abc1234',callsign='N0CALL')]
        mc = int(command.split()[1]); policy = 'page' if command.endswith('pagewake') else 'bank'
        if mc == 15 and policy == 'bank': raise TimeoutError('injected timeout')
        return [dict(cmd='dvbpace',modcod=mc,queue_wake_policy=policy,paced_wifi_up=True,encoder_pie=15,
                     bch_variant='psram-slices',bch_slice_bytes=24576,
                     fills=1,jitter=dict(complete=True,banks_by_irq_count=[dict(calls=1,histogram=[0,0,0,0,1,0])],
                     worst_bank=dict(cycles=40138 if mc == 13 else 40000)),
                     status='ESP_ERR_TIMEOUT' if mc == 17 and policy == 'page' else 'ESP_OK',
                     encoder_failed=False,queue_empty=0,underruns=0,late_fills=0,late_switches=0,
                     fatal_late=0,min_fill_slack_cycles=1,max_fill_cycles=40138 if mc == 13 else 40000,
                     encode_cycles_mean=193848 if mc == 11 else 169784,wifi_min_free=20000,
                     queue_wakes=1,queue_low_pages=1)]

    summary = check.suite(send,'abc1234',25)
    assert sent == ['info']+[command for *_,command in check.commands(25)]
    assert [r['verdict'] for r in summary] == ['OBSERVED','FAIL','OBSERVED','FAIL','OBSERVED','PASS','OBSERVED','PASS']
    assert len(summary) == 8 and summary[-1]['modcod'] == 11
    plan = subprocess.check_output(['python3',str(ROOT/'host/jitter.py'),'/unused','--plan'],text=True)
    assert len(plan.splitlines()) == 9 and all('dvbpace' in s for s in plan.splitlines()[1:])
    print('jitter coordinator: fixed ceilings, both policies, keeps going after timing/transport faults: ok')


if __name__ == '__main__': main()
