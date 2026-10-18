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
        assert r['worst_bank']['notify_calls'] == r['worst_bank']['notify_cycles'] == 0
    print('jitter observer: nested handlers, original arguments, teardown, wraps and bin edges: ok')
    spec = importlib.util.spec_from_file_location('jitter_check',ROOT/'host/jitter.py')
    check = importlib.util.module_from_spec(spec); spec.loader.exec_module(check)
    sent = []

    def send(command):
        sent.append(command)
        if command == 'info': return [dict(firmware='abc1234',callsign='N0CALL')]
        args = command.split(); mc = int(args[1]); policy = 'bank' if 'bankwake' in args else 'page' if 'pagewake' in args or mc == 11 else 'bank'
        observer = 'observe' in args
        if mc == 15 and not observer: raise TimeoutError('injected timeout')
        r = dict(cmd='dvbpace',modcod=mc,queue_wake_policy=policy,paced_wifi_up=True,encoder_pie=15,
                     bch_variant='resident-pair' if mc in (11,17) else 'split-pair',warmup_frames=3,
                     encoder_priority=24,
                     fills=1,jitter_enabled=observer,seconds=int(args[3]),
                     elapsed_samples=2400000000,planned_samples=2400000000,
                     status='ESP_ERR_TIMEOUT' if mc == 17 and policy == 'page' else 'ESP_OK',
                     encoder_failed=False,queue_empty=0,underruns=0,late_fills=0,late_switches=0,
                     fatal_late=0,min_fill_slack_cycles=1,max_fill_cycles=40138 if mc == 13 else 40000,
                     encode_cycles_mean=193848 if mc == 11 else 160000,wifi_min_free=20000,
                     queue_wakes=1,queue_wake_max_cycles=100,
                     min_lead_symbols=3000,encode_cycles_live_max=200000,queue_low_pages=1,prefill_frames=1,encoder_stack_min_free=512)
        if observer:
            r['jitter'] = dict(complete=True,banks_by_irq_count=[dict(calls=1,histogram=[0,0,0,0,1,0])],
                              worst_bank=dict(cycles=r['max_fill_cycles'],notify_calls=2,
                                              notify_cycles=9000,
                                              notify_max_cycles=7000))
        return [r]

    summary = check.suite(send,'abc1234',20)
    assert sent == ['info']+[command for *_,command in check.commands(20)]
    assert [r['verdict'] for r in summary] == ['FAIL','OBSERVED','OBSERVED','FAIL','OBSERVED','OBSERVED',
                                            'PASS','OBSERVED','OBSERVED','PASS','OBSERVED','OBSERVED']
    assert len(summary) == 12 and summary[-1]['modcod'] == 11
    r, = send('dvbpace 17 26m667 20 128 0')
    r['elapsed_samples'] -= 1
    try: check.gate(r,17)
    except AssertionError as e: assert str(e) == 'incomplete pace'
    else: raise AssertionError('accepted a truncated run')
    r['elapsed_samples'] += 1
    r['min_lead_symbols'] = 2999
    try: check.gate(r,17)
    except AssertionError as e: assert 'lead' in str(e)
    else: raise AssertionError('accepted lead below 3000 symbols')
    r['min_lead_symbols'] = 3000
    r['encoder_priority'] = 5
    try: check.timing([r],17,'bank',False)
    except AssertionError as e: assert str(e) == 'wrong encoder priority'
    else: raise AssertionError('accepted the retired polling priority')
    r['encoder_priority'] = 24
    r['jitter_enabled'] = True
    try: check.timing([r],17,'bank',False)
    except AssertionError as e: assert str(e) == 'wrong observer state'
    else: raise AssertionError('accepted observer in clean gate')
    plan = subprocess.check_output(['python3',str(ROOT/'host/jitter.py'),'/unused','--plan'],text=True)
    assert len(plan.splitlines()) == 13 and all('dvbpace' in s for s in plan.splitlines()[1:])
    print('jitter coordinator: clean gate, same-bank notify times, keeps going after timing/transport faults: ok')


if __name__ == '__main__': main()
