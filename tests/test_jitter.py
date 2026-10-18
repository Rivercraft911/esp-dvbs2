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
        if mc == 15 and not observer and 'trace' not in args: raise TimeoutError('injected timeout')
        r = dict(cmd='dvbpace',modcod=mc,queue_wake_policy=policy,paced_wifi_up=True,encoder_pie=15,
                     bch_variant='resident-pair' if mc in (11,17) else 'split-pair',warmup_frames=3,
                     encoder_priority=24,admission_pages=10,admission_wait_pages=26,admission_update_max_cycles=7000,queue_stream_page_max_cycles=600,
                     dummy_startup=4,dummy_steady=2,dummy_alignment=3,
                     handoff=[dict(words=8100,shape_cycles=17000),dict(words=6092,shape_cycles=13000)],
                     fills=1,frames_played=24000,jitter_enabled=observer,seconds=int(args[3]),
                     queue_pages=int(next((a[6:] for a in args if a.startswith('pages=')),'46')),
                     free_internal=40000,min_free_internal=30000,
                     elapsed_samples=2400000000,planned_samples=2400000000,
                     status='ESP_ERR_TIMEOUT' if mc == 17 and policy == 'page' else 'ESP_OK',
                     encoder_failed=False,queue_empty=0,underruns=0,late_fills=0,late_switches=0,
                     fatal_late=0,min_fill_slack_cycles=1,max_fill_cycles=40138 if mc == 13 else 40000,
                     encode_cycles_mean=193848 if mc == 11 else 160000,wifi_min_free=20000,
                     queue_wakes=1,queue_wake_max_cycles=100,
                     min_lead_symbols=3000,encode_cycles_live_max=200000,queue_low_pages=1,prefill_frames=1,encoder_stack_min_free=512)
        if 'nowifi' in args:
            r['paced_wifi_up'] = False
            del r['wifi_min_free']
            if r['queue_pages'] == 50: r.update(status='ESP_ERR_TIMEOUT',late_fills=1)
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
    r['min_lead_symbols'] = 0
    r['encode_cycles_mean'] = 300000
    check.gate(r,17) # slow encoder and reported steady dummies are allowed
    r['min_lead_symbols'] = 2999
    try: check.gate(r,11)
    except AssertionError as e: assert 'lead' in str(e)
    else: raise AssertionError('accepted lead below 3000 symbols')
    assert not summary[6]['steady_dummy_target_met']
    r['min_lead_symbols'] = 3000
    try: check.gate(r,11)
    except AssertionError as e: assert 'encoder' in str(e)
    else: raise AssertionError('accepted a slow qualified QPSK encoder')
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
    # Trace mode must also continue after an incomplete/failed first capture.
    summary = check.suite(send,'abc1234',20,trace=True)
    assert len(summary) == 2 and [r['modcod'] for r in summary] == [15,17]
    assert all(r['verdict'] == 'FAIL' and 'stream_trace' in r['detail'] for r in summary)
    trace_summary = check.trace_summary
    check.trace_summary = lambda r: dict(frames=64) # framing checked by test_stream_trace
    summary = check.suite(send,'abc1234',20,trace=True)
    check.trace_summary = trace_summary
    assert all(r['verdict'] == 'CAPTURED' and not r['observer'] for r in summary)
    assert sent[-2:] == ['dvbpace 15 26m667 10 128 0 trace','dvbpace 17 26m667 10 128 0 trace']
    summary = check.suite(send,'abc1234',20,page_sweep=True)
    assert [r['queue_pages'] for r in summary] == [46,50,54,58]
    assert [r['verdict'] for r in summary] == ['SWEEP','FAIL','SWEEP','SWEEP']
    assert all(not r['paced_wifi_up'] and r['dummy_steady'] == 2 and r['frames_played'] == 24000 for r in summary)
    assert all('trace' not in r and not r['observer'] for r in summary)
    plan = subprocess.check_output(['python3',str(ROOT/'host/jitter.py'),'/unused','--page-sweep','--plan'],text=True)
    assert plan.splitlines() == ['info (reset once)']+[
        f'dvbpace 17 26m667 10 128 0 pages={pages} nowifi' for pages in (46,50,54,58)]
    plan = subprocess.check_output(['python3',str(ROOT/'host/jitter.py'),'/unused','--queue-gate','--plan'],text=True)
    expected = [c for mc in (13,15,17) for c in (f'wifimem {mc} 26m667',f'dvbpace {mc} 26m667 20 128 0')]
    assert plan.splitlines() == ['info (reset once)']+expected
    queue_sent = []

    def queue_send(command):
        queue_sent.append(command)
        if command == 'info': return [dict(firmware='abc1234',callsign='N0CALL')]
        mc = int(command.split()[1])
        r, = send(f'dvbpace {mc} 26m667 20 128 0 trace') # reuse complete record without injected transport timeout
        fixture = command.startswith('dvbpace')
        r.update(cmd='dvbpace' if fixture else 'wifimem',queue_pages=54,
                 input_memory='psram' if fixture else 'none',reference_psram_bytes=7000 if fixture else 0,
                 ring_heap_bytes=0,workspace_heap_bytes=8144,render_warm_cycles=80000,
                 largest_internal=16000,wifi_largest_up=15000,
                 layout=dict(fast='0x3fce4fb0',workspace='0x3fcbdab0',encoder_tables='0x3fcb5d90'),
                 max_fill_sample=417739472,dummy_startup_frame=2,
                 queue_layout=dict(contiguous=True,page_bytes=528,resident_pages=54,
                                   regions=[dict(first_page=0,pages=54,begin='0x3fcab500',end='0x3fcb2460',sram_levels=[4,5],dac_overlap=False)]),
                 dummy_steady=2 if mc == 15 and fixture else 0,max_fill_cycles=40000,
                 queue_sizing=dict(automatic=True,ready=True,target_pages=54,floor_bytes=20000,
                                   guard_bytes=2048,free_before=32000,free_after=23520,min_before=32000))
        return [r]

    summary = check.queue_suite(queue_send,'abc1234')
    assert queue_sent == ['info']+expected
    assert [r['verdict'] for r in summary] == ['SETUP','PASS','SETUP','FAIL','SETUP','PASS']
    assert all(r['queue_pages'] == 54 and r['queue_sizing']['free_after'] >= 22048 for r in summary)
    assert all(r['wifi_largest_up'] == 15000 and 'workspace' in r['layout'] for r in summary)
    def failed_cache(command):
        if command.startswith('wifimem 15'):
            return [dict(cmd='wifimem',error='encoder cache allocation failed',requested_bytes=24112,
                         free_internal=40000,largest_internal=20000)]
        records = queue_send(command)
        if command.startswith('dvbpace 17'):
            records[0].update(status='ESP_ERR_TIMEOUT',error='paced run failed')
        return records
    summary = check.queue_suite(failed_cache,'abc1234')
    assert summary[2]['detail'] == 'encoder cache allocation failed' and summary[2]['largest_internal'] == 20000
    assert summary[-2]['verdict'] == 'SETUP'
    assert summary[-1]['verdict'] == 'FAIL' and summary[-1]['frames_played'] == 24000
    assert 'workspace' in summary[-1]['layout']
    print('jitter coordinator: clean gate, same-bank notify times, keeps going after timing/transport faults: ok')


if __name__ == '__main__': main()
