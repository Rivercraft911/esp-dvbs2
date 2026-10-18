#!/usr/bin/env python3
"""Radio-off A/B: original page wakes versus bank-end wakes, about 55 s per mode.

No flashing or transmitter commands. Keep the Mac awake, for example:
  caffeinate -i python3 host/jitter.py notes/eight-lofty-round3
"""
import argparse
import datetime as dt
import json
import pathlib
import subprocess
import sys

CASES = ((13,'26m667'),(15,'26m667'),(17,'26m667'),(11,'33m333'))


def commands(seconds):
    return [(mc,rate,policy,f'dvbpace {mc} {rate} {seconds} 128 0'+
             (' pagewake' if policy == 'page' else ''))
            for mc,rate in CASES for policy in ('page','bank')]


def timing(rows, mc, policy):
    r, = [r for r in rows if r.get('cmd') == 'dvbpace']
    assert r['modcod'] == mc and r['queue_wake_policy'] == policy, 'wrong mode/policy'
    assert r['paced_wifi_up'] and r['encoder_pie'] == 15, 'Wi-Fi/PIE missing'
    if mc in (13,15):
        assert r['bch_variant'] == 'psram-slices' and r['bch_slice_bytes'] == 24576, 'wrong BCH variant'
    j = r['jitter']
    assert j['complete'], 'interrupt observer capacity exceeded'
    assert sum(b['calls'] for b in j['banks_by_irq_count']) == r['fills'], 'missing banks'
    assert all(sum(b['histogram']) == b['calls'] for b in j['banks_by_irq_count']), 'bad histogram'
    assert j['worst_bank']['cycles'] == r['max_fill_cycles'], 'wrong worst bank'
    return r


def gate(r, mc):
    assert r['status'] == 'ESP_OK', r['status']
    assert not r['encoder_failed'] and r['queue_empty'] == r['underruns'] == 0, 'encoder/queue fault'
    assert r['late_fills'] == r['late_switches'] == r['fatal_late'] == 0, 'late bank'
    assert r['min_fill_slack_cycles'] > 0, 'no fill slack'
    assert r['max_fill_cycles'] <= 40137, f"bank {r['max_fill_cycles']} > 40137 cycles"
    ceiling = 193848 if mc == 11 else 169784
    assert r['encode_cycles_mean'] <= ceiling, f"encoder {r['encode_cycles_mean']} > {ceiling} cycles/frame"
    assert r['wifi_min_free'] >= 20000, f"Wi-Fi minimum {r['wifi_min_free']} < 20000 B"


def suite(send, firmware, seconds):
    info, = send('info')
    assert info['firmware'] == firmware and info['callsign'] == 'N0CALL', 'wrong firmware/callsign'
    summary = []
    for mc,rate,policy,command in commands(seconds):
        row = dict(command=command,modcod=mc,policy=policy)
        try:
            r = timing(send(command),mc,policy)
            row.update(status=r['status'],max_fill_cycles=r['max_fill_cycles'],
                       encode_cycles_mean=r['encode_cycles_mean'],
                       queue_wakes=r['queue_wakes'],queue_low_pages=r['queue_low_pages'],
                       worst_bank=r['jitter']['worst_bank'])
            # The original path may reproduce the fault. Its measurements still
            # count as evidence; the candidate alone must satisfy every gate.
            if policy == 'bank': gate(r,mc)
            row['verdict'] = 'PASS' if policy == 'bank' else 'OBSERVED'
        except Exception as e:
            row.update(verdict='FAIL',detail=str(e))
        summary.append(row)
        print(f"{row['verdict']}: {mc} {rate} {policy}: "+
              (f"bank {row['max_fill_cycles']} cycles, encoder {row['encode_cycles_mean']} cycles/frame"
               if 'max_fill_cycles' in row else row.get('detail',''))+
              (f"; {row['detail']}" if 'max_fill_cycles' in row and 'detail' in row else ''),flush=True)
    return summary


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('output',type=pathlib.Path)
    p.add_argument('--seconds',type=int,default=25,choices=range(1,56),metavar='1..55')
    p.add_argument('--port',default='/dev/cu.usbmodem101')
    p.add_argument('--plan',action='store_true',help='print commands without opening/resetting a port')
    a = p.parse_args()
    if a.plan:
        print('info (reset once)')
        for *_,command in commands(a.seconds): print(command)
        return 0
    import board
    firmware = subprocess.check_output(['git','rev-parse','--short','HEAD'],text=True).strip()
    a.output.mkdir(parents=True,exist_ok=False)
    port = board.resolve_port(a.port)

    def send(command):
        log = a.output/(command.replace(' ','-')+'.log')
        try:
            rows = board.run(port,command,timeout=a.seconds+30,reset=command == 'info')
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
        summary = suite(send,firmware,a.seconds)
    (a.output/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    print('Records:',a.output)
    return int(any(r['verdict'] == 'FAIL' for r in summary))


if __name__ == '__main__': sys.exit(main())
