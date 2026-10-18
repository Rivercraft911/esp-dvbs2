#!/usr/bin/env python3
"""Radio-off clean gate plus polling/page/bank wake diagnosis, about one minute per mode.

No flashing or transmitter commands. Keep the Mac awake, for example:
  caffeinate -i python3 host/jitter.py notes/eight-lofty-round5
"""
import argparse
import datetime as dt
import json
import pathlib
import subprocess
import sys

CASES = ((13,'26m667'),(15,'26m667'),(17,'26m667'),(11,'33m333'))


def commands(seconds, observe_seconds=5):
    rows = []
    for mc,rate in CASES:
        default = 'page' if mc == 11 else 'poll'
        comparison = 'bank' if mc == 11 else 'page'
        for policy,observer,duration in ((default,False,seconds),(default,True,observe_seconds),
                                         (comparison,True,observe_seconds)):
            flag = '' if policy == default else ' '+policy+'wake'
            rows.append((mc,rate,policy,observer,f'dvbpace {mc} {rate} {duration} 128 0'+
                         flag+(' observe' if observer else '')))
    return rows


def timing(rows, mc, policy, observer):
    r, = [r for r in rows if r.get('cmd') == 'dvbpace']
    assert r['modcod'] == mc and r['queue_wake_policy'] == policy, 'wrong mode/policy'
    assert r['paced_wifi_up'] and r['encoder_pie'] == 15, 'Wi-Fi/PIE missing'
    assert r['bch_variant'] == ('resident-pair' if mc in (11,17) else 'split-pair'), 'wrong BCH variant'
    assert r['warmup_frames'] == 3, 'encoder warm-up missing'
    assert r['jitter_enabled'] == observer and ('jitter' in r) == observer, 'wrong observer state'
    if policy == 'poll':
        assert r['queue_wakes'] == r['queue_wake_max_cycles'] == 0, 'polling called the kernel'
    if observer:
        j = r['jitter']
        assert j['complete'], 'interrupt observer capacity exceeded'
        assert sum(b['calls'] for b in j['banks_by_irq_count']) == r['fills'], 'missing banks'
        assert all(sum(b['histogram']) == b['calls'] for b in j['banks_by_irq_count']), 'bad histogram'
        w = j['worst_bank']
        assert w['cycles'] == r['max_fill_cycles'], 'wrong worst bank'
        assert 0 <= w['notify_max_cycles'] <= w['notify_cycles'] <= w['cycles'], 'bad notify timing'
        assert bool(w['notify_calls']) == bool(w['notify_max_cycles']), 'missing notify timing'
        if policy == 'poll': assert w['notify_calls'] == 0, 'polling bank notified'
    return r


def gate(r, mc):
    assert r['status'] == 'ESP_OK', r['status']
    assert r['seconds'] >= 20 and r['elapsed_samples'] >= r['planned_samples'], 'incomplete pace'
    assert not r['encoder_failed'] and r['queue_empty'] == r['underruns'] == 0, 'encoder/queue fault'
    assert r['late_fills'] == r['late_switches'] == r['fatal_late'] == 0, 'late bank'
    assert r['min_fill_slack_cycles'] > 0, 'no fill slack'
    assert r['max_fill_cycles'] <= 40137, f"bank {r['max_fill_cycles']} > 40137 cycles"
    assert r['min_lead_symbols'] >= 3000, f"lead {r['min_lead_symbols']} < 3000 symbols"
    ceiling = 193848 if mc == 11 else 160000
    assert r['encode_cycles_mean'] <= ceiling, f"encoder {r['encode_cycles_mean']} > {ceiling} cycles/frame"
    assert r['wifi_min_free'] >= 20000, f"Wi-Fi minimum {r['wifi_min_free']} < 20000 B"
    assert r['encoder_stack_min_free'] >= 512, 'encoder stack < 512 B'


def suite(send, firmware, seconds, observe_seconds=5):
    info, = send('info')
    assert info['firmware'] == firmware and info['callsign'] == 'N0CALL', 'wrong firmware/callsign'
    summary = []
    for mc,rate,policy,observer,command in commands(seconds,observe_seconds):
        row = dict(command=command,modcod=mc,policy=policy,observer=observer)
        try:
            r = timing(send(command),mc,policy,observer)
            row.update(status=r['status'],max_fill_cycles=r['max_fill_cycles'],
                       encode_cycles_mean=r['encode_cycles_mean'],
                       queue_wakes=r['queue_wakes'],queue_low_pages=r['queue_low_pages'],
                       queue_empty=r['queue_empty'],prefill_frames=r['prefill_frames'],
                       warmup_frames=r['warmup_frames'],wifi_min_free=r['wifi_min_free'],
                       min_lead_symbols=r['min_lead_symbols'],encode_cycles_live_max=r['encode_cycles_live_max'])
            if mc != 11: row['keyed_reserve_target_met'] = r['encode_cycles_mean'] <= 132000
            if observer: row['worst_bank'] = r['jitter']['worst_bank']
            # Instrumented runs diagnose the lock; only the clean default run is
            # admitted. Keep faults in the raw records and always finish the suite.
            if not observer: gate(r,mc)
            row['verdict'] = 'OBSERVED' if observer else 'PASS'
        except Exception as e:
            row.update(verdict='FAIL',detail=str(e))
        summary.append(row)
        print(f"{row['verdict']}: {mc} {rate} {policy} {'observed' if observer else 'clean'}: "+
              (f"bank {row['max_fill_cycles']} cycles, encoder {row['encode_cycles_mean']} cycles/frame"
               if 'max_fill_cycles' in row else row.get('detail',''))+
              (f"; lead {row['min_lead_symbols']} symbols, {row['status']}" if 'status' in row else '')+
              ('; 132k reserve target '+('met' if row['keyed_reserve_target_met'] else 'open')
               if 'keyed_reserve_target_met' in row else '')+
              (f"; notify max {row['worst_bank']['notify_max_cycles']} cycles in worst bank"
               if 'worst_bank' in row else '')+
              (f"; {row['detail']}" if 'max_fill_cycles' in row and 'detail' in row else ''),flush=True)
    return summary


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('output',type=pathlib.Path)
    p.add_argument('--seconds',type=int,default=20,choices=range(20,56),metavar='20..55')
    p.add_argument('--observe-seconds',type=int,default=5,choices=range(1,21),metavar='1..20')
    p.add_argument('--port',default='/dev/cu.usbmodem101')
    p.add_argument('--plan',action='store_true',help='print commands without opening/resetting a port')
    a = p.parse_args()
    if a.plan:
        print('info (reset once)')
        for *_,command in commands(a.seconds,a.observe_seconds): print(command)
        return 0
    import board
    firmware = subprocess.check_output(['git','rev-parse','--short','HEAD'],text=True).strip()
    a.output.mkdir(parents=True,exist_ok=False)
    port = board.resolve_port(a.port)

    def send(command):
        log = a.output/(command.replace(' ','-')+'.log')
        try:
            rows = board.run(port,command,timeout=max(a.seconds,a.observe_seconds)+30,reset=command == 'info')
        except Exception as e:
            log.write_text(str(e)+'\n')
            raise RuntimeError(f'{type(e).__name__}; see {log}') from e
        stamp = dt.datetime.now(dt.timezone.utc).isoformat()
        log.write_text('\n'.join(json.dumps(r) for r in rows)+'\n')
        with (a.output/'runs.jsonl').open('a') as f:
            for r in rows:
                f.write(json.dumps(dict(host_time=stamp,command=command,**r))+'\n')
        return rows

    with board.reserve_port(port,reset=True):
        summary = suite(send,firmware,a.seconds,a.observe_seconds)
    (a.output/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    print('Records:',a.output)
    return int(any(r['verdict'] == 'FAIL' for r in summary))


if __name__ == '__main__': sys.exit(main())
