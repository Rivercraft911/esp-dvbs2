#!/usr/bin/env python3
"""Radio-off clean gate plus bank/page wake diagnosis, about one minute per mode.

No flashing or transmitter commands. Keep the Mac awake, for example:
  caffeinate -i python3 host/jitter.py notes/eight-lofty-round15
"""
import argparse
import datetime as dt
import json
import pathlib
import subprocess
import sys

CASES = ((13,'26m667'),(15,'26m667'),(17,'26m667'),(11,'33m333'))


def commands(seconds, observe_seconds=5, trace=False, page_sweep=False, queue_gate=False):
    if queue_gate:
        return [(mc,'26m667','bank',False,command) for mc in (13,15,17)
                for command in (f'wifimem {mc} 26m667',f'dvbpace {mc} 26m667 20 128 0')]
    if page_sweep:
        return [(17,'26m667','bank',False,f'dvbpace 17 26m667 10 128 0 pages={pages} nowifi')
                for pages in (46,50,54,58)]
    if trace:
        return [(mc,'26m667','bank',False,f'dvbpace {mc} 26m667 10 128 0 trace')
                for mc in (15,17)]
    rows = []
    for mc,rate in CASES:
        default = 'page' if mc == 11 else 'bank'
        comparison = 'bank' if mc == 11 else 'page'
        for policy,observer,duration in ((default,False,seconds),(default,True,observe_seconds),
                                         (comparison,True,observe_seconds)):
            flag = '' if policy == default else ' '+policy+'wake'
            rows.append((mc,rate,policy,observer,f'dvbpace {mc} {rate} {duration} 128 0'+
                         flag+(' observe' if observer else '')))
    return rows


def timing(rows, mc, policy, observer, wifi=True):
    r, = [r for r in rows if r.get('cmd') == 'dvbpace']
    assert r['modcod'] == mc and r['queue_wake_policy'] == policy, 'wrong mode/policy'
    assert r['paced_wifi_up'] == wifi and r['encoder_pie'] == 15, 'wrong Wi-Fi/PIE state'
    assert r['bch_variant'] == ('resident-pair' if mc in (11,17) else 'split-pair'), 'wrong BCH variant'
    assert r['encoder_priority'] == 24, 'wrong encoder priority'
    assert r['warmup_frames'] == 3, 'encoder warm-up missing'
    assert r['jitter_enabled'] == observer and ('jitter' in r) == observer, 'wrong observer state'
    if mc != 11:
        assert 9 <= r['admission_pages'] <= r['admission_wait_pages'] <= 44, 'wrong streaming admission'
        assert r['queue_stream_page_max_cycles'] >= 0, 'missing production bound'
        assert r['admission_update_max_cycles'] >= 0, 'missing calibration timing'
        assert len(r['handoff']) == 2, 'missing handoff record'
        if r['status'] == 'ESP_OK':
            assert [p['words'] for p in r['handoff']] == [8100,6092], 'wrong ID/data handoff'
            assert all(p['shape_cycles'] > 0 for p in r['handoff']), 'missing handoff timing'
        assert all(r[k] >= 0 for k in ('dummy_startup','dummy_steady','dummy_alignment')), 'bad dummy counts'
    if observer:
        j = r['jitter']
        assert j['complete'], 'interrupt observer capacity exceeded'
        assert sum(b['calls'] for b in j['banks_by_irq_count']) == r['fills'], 'missing banks'
        assert all(sum(b['histogram']) == b['calls'] for b in j['banks_by_irq_count']), 'bad histogram'
        w = j['worst_bank']
        assert w['cycles'] == r['max_fill_cycles'], 'wrong worst bank'
        assert 0 <= w['notify_max_cycles'] <= w['notify_cycles'] <= w['cycles'], 'bad notify timing'
        assert bool(w['notify_calls']) == bool(w['notify_max_cycles']), 'missing notify timing'
    return r


def gate(r, mc):
    assert r['status'] == 'ESP_OK', r['status']
    assert r['seconds'] >= 20 and r['elapsed_samples'] >= r['planned_samples'], 'incomplete pace'
    assert not r['encoder_failed'] and r['queue_empty'] == r['underruns'] == 0, 'encoder/queue fault'
    assert r['late_fills'] == r['late_switches'] == r['fatal_late'] == 0, 'late bank'
    assert r['min_fill_slack_cycles'] > 0, 'no fill slack'
    assert r['max_fill_cycles'] <= 40137, f"bank {r['max_fill_cycles']} > 40137 cycles"
    if mc == 11:
        assert r['min_lead_symbols'] >= 3000, f"lead {r['min_lead_symbols']} < 3000 symbols"
        assert r['encode_cycles_mean'] <= 193848, f"encoder {r['encode_cycles_mean']} > 193848 cycles/frame"
    assert r['wifi_min_free'] >= 20000, f"Wi-Fi minimum {r['wifi_min_free']} < 20000 B"
    assert r['encoder_stack_min_free'] >= 512, 'encoder stack < 512 B'


def trace_summary(r):
    t = r['stream_trace']
    assert t['version'] == 2 and t['complete'], 'incomplete stream trace; retain raw rows'
    first = t['first_frame']
    assert first == 68 and t['frame_count'] == t['frames_complete'] == 64
    assert t['storage'] == 'psram' and t['external'], 'trace ring is not external'
    allocation = t['allocation']
    assert allocation['frame'] == 66 and allocation['queue_pages'] == r['queue_pages']
    queued = (allocation['produced']-allocation['consumed']) & 0xffffffff
    assert queued <= r['queue_pages'] and allocation['frame']-(queued+43)//44 >= 64, 'trace allocated before steady state'
    setup = r['trace_setup']
    assert setup['state'] == 'allocated' and setup['allocate_frame'] == 66 and setup['min_played_frames'] == 64
    producer, fill = t['lanes']
    assert [producer['core'],fill['core']] == [0,1]
    for lane in (producer,fill):
        assert lane['armed'], 'trace did not arm'
        assert not lane['dropped'], 'stream trace overflow'
        assert all(len(e) == 8 for e in lane['rows']), 'bad trace row'
        assert all(a[0] <= b[0] for a,b in zip(lane['rows'],lane['rows'][1:])), 'trace clock reversed'
    waits = {3:[],5:[]}
    dummies = []
    for frame in range(first,first+64):
        p = [e for e in producer['rows'] if e[2] == frame]
        f = [e for e in fill['rows'] if e[2] == frame]
        assert sum(e[3] == 13 for e in p) == 1, 'missing frame start'
        assert sum(e[3] == 1 for e in p) == sum(e[3] == 2 for e in p) == 1, 'missing FEC edge'
        published = [(e[4],e[6]) for e in p if e[3] == 7]
        released = [(e[5],e[6]) for e in f if e[3] == 9]
        assert len(published) == 44 and published == released, 'page publication/release mismatch'
        assert sum(e[3] == 11 for e in f) == 1, 'missing data frame end'
        data = [e for e in f if e[3] == 8 and not e[6] & (1<<19)]
        assert len(data) == 1, 'missing data admission'
        dummies.append(data[0][7])
        pending = {}
        for e in p:
            kind = e[3]
            if kind in waits:
                assert kind not in pending, 'nested page wait'
                pending[kind] = e
            elif kind in (4,6):
                start = pending.pop(kind-1)
                assert e[6:] == start[6:], 'wait changed page index/mapper position'
                waits[kind-1].append(e[0]-start[0])
        assert not pending, 'unclosed page wait'
    return dict(frames=64,address=t['address'],allocation_frame=allocation['frame'],
                internal_delta=allocation['internal_after']-allocation['internal_before'],
                producer_events=len(producer['rows']),fill_events=len(fill['rows']),
                map_waits=len(waits[3]),map_wait_max_us=max(waits[3],default=0),
                pl_waits=len(waits[5]),pl_wait_max_us=max(waits[5],default=0),
                dummies_before_data=sum(dummies),max_dummies_before_data=max(dummies),
                anchor_span_cycles=[(p['anchor']['cycles_after']-p['anchor']['cycles_before']) & 0xffffffff
                                    for p in t['lanes']])


def suite(send, firmware, seconds, observe_seconds=5, trace=False, page_sweep=False):
    info, = send('info')
    assert info['firmware'] == firmware and info['callsign'] == 'N0CALL', 'wrong firmware/callsign'
    summary = []
    for mc,rate,policy,observer,command in commands(seconds,observe_seconds,trace,page_sweep):
        row = dict(command=command,modcod=mc,policy=policy,observer=observer)
        try:
            r = timing(send(command),mc,policy,observer,wifi=not page_sweep)
            row.update(status=r['status'],max_fill_cycles=r['max_fill_cycles'],
                       encode_cycles_mean=r['encode_cycles_mean'],
                       queue_wakes=r['queue_wakes'],queue_low_pages=r['queue_low_pages'],
                       queue_empty=r['queue_empty'],prefill_frames=r['prefill_frames'],
                       warmup_frames=r['warmup_frames'],
                       frames_played=r['frames_played'],
                       min_lead_symbols=r['min_lead_symbols'],encode_cycles_live_max=r['encode_cycles_live_max'])
            if page_sweep:
                pages = int(next(a[6:] for a in command.split() if a.startswith('pages=')))
                row.update(queue_pages=r['queue_pages'],paced_wifi_up=r['paced_wifi_up'],
                           free_internal=r['free_internal'],min_free_internal=r['min_free_internal'])
                assert r['queue_pages'] == pages, 'wrong page count'
            else: row['wifi_min_free'] = r['wifi_min_free']
            if 'queue_fault' in r: row['queue_fault'] = r['queue_fault']
            if mc != 11:
                row.update({k:r[k] for k in ('dummy_startup','dummy_steady','dummy_alignment')})
                if 'handoff' in r: row['handoff'] = r['handoff']
                row['steady_dummy_target_met'] = r['dummy_steady'] == 0
            if observer: row['worst_bank'] = r['jitter']['worst_bank']
            if trace: row['trace'] = trace_summary(r)
            # Instrumented runs diagnose the lock; only the clean default run is
            # admitted. Keep faults in the raw records and always finish the suite.
            if page_sweep:
                assert r['status'] == 'ESP_OK', r['status']
                assert r['seconds'] == 10 and r['elapsed_samples'] >= r['planned_samples'], 'incomplete sweep run'
                assert not r['encoder_failed'] and r['queue_empty'] == r['underruns'] == 0, 'encoder/queue fault'
                assert r['late_fills'] == r['late_switches'] == r['fatal_late'] == 0, 'late bank'
            elif not (observer or trace): gate(r,mc)
            row['verdict'] = 'SWEEP' if page_sweep else 'CAPTURED' if trace else 'OBSERVED' if observer else 'PASS'
        except Exception as e:
            row.update(verdict='FAIL',detail=str(e))
        summary.append(row)
        print(f"{row['verdict']}: {mc} {rate} {policy} {'Wi-Fi off sweep' if page_sweep else 'trace' if trace else 'observed' if observer else 'clean'}: "+
              (f"bank {row['max_fill_cycles']} cycles, encoder {row['encode_cycles_mean']} cycles/frame"
               if 'max_fill_cycles' in row else row.get('detail',''))+
              (f"; data frames {row['frames_played']}, lead {row['min_lead_symbols']} symbols, {row['status']}" if 'status' in row else '')+
              (f"; {row['queue_pages']} pages" if 'queue_pages' in row else '')+
              (f"; dummies startup {row['dummy_startup']}, steady {row['dummy_steady']} (target 0), alignment {row['dummy_alignment']}"
               if 'dummy_startup' in row else '')+
              (f"; handoff {row['handoff']}" if 'handoff' in row else '')+
              (f"; notify max {row['worst_bank']['notify_max_cycles']} cycles in worst bank"
               if 'worst_bank' in row else '')+
              (f"; trace {row['trace']}" if 'trace' in row else '')+
              (f"; {row['detail']}" if 'max_fill_cycles' in row and 'detail' in row else ''),flush=True)
    return summary


def queue_suite(send, firmware):
    """Unkeyed keyed-layout probe, then the clean 20-second Wi-Fi-up gate."""
    info, = send('info')
    assert info['firmware'] == firmware and info['callsign'] == 'N0CALL', 'wrong firmware/callsign'
    summary = []
    for mc,rate,policy,observer,command in commands(20,queue_gate=True):
        row = dict(command=command,modcod=mc,setup='keyed' if command.startswith('wifimem') else 'pace')
        try:
            r, = [r for r in send(command) if r.get('cmd') in ('wifimem','dvbpace')]
            if 'status' not in r and 'error' in r:
                row.update({k:r[k] for k in ('requested_bytes','free_internal','largest_internal') if k in r})
                raise RuntimeError(r['error'])
            row.update(status=r['status'],queue_pages=r['queue_pages'],wifi_min_free=r['wifi_min_free'],
                       queue_sizing=r['queue_sizing'],queue_layout=r['queue_layout'],
                       layout=r['layout'],largest_internal=r['largest_internal'],wifi_largest_up=r['wifi_largest_up'],
                       reference_psram_bytes=r['reference_psram_bytes'],render_warm_cycles=r['render_warm_cycles'])
            if row['setup'] == 'pace':
                row.update({k:r[k] for k in ('frames_played','dummy_startup','dummy_steady','dummy_alignment',
                            'max_fill_cycles','max_fill_sample','dummy_startup_frame','min_lead_symbols','encode_cycles_mean')})
                timing([r],mc,policy,observer)
                gate(r,mc)
                assert r['dummy_steady'] == 0, f"steady dummies {r['dummy_steady']} (target 0)"
                assert r['input_memory'] == 'psram' and r['reference_psram_bytes'] > 0, 'wrong pace reference placement'
            else:
                assert r['cmd'] == 'wifimem' and r['modcod'] == mc, 'wrong keyed-layout probe'
                assert r['status'] == 'ESP_OK', r['status']
                assert not r['reference_psram_bytes'] and r['input_memory'] == 'none', 'probe has pace fixture'
            q = r['queue_sizing']
            assert q['automatic'] and q['ready'] and r['queue_pages'] == q['target_pages'] == 54, 'target queue not available'
            assert q['floor_bytes'] == 20000 and q['min_before'] >= 20000, 'startup heap below floor'
            assert q['guard_bytes'] == 2048, 'wrong Wi-Fi reserve'
            assert q['free_after'] >= q['floor_bytes']+q['guard_bytes'], 'queue consumed heap reserve'
            assert r['wifi_min_free'] >= 20000, 'Wi-Fi heap below floor'
            layout = r['queue_layout']
            region, = layout['regions']
            assert layout['contiguous'] and layout['resident_pages'] == 54 and layout['page_bytes'] == 528, 'queue split'
            assert region['first_page'] == 0 and region['pages'] == 54 and not region['dac_overlap'], 'wrong queue region'
            begin, end = int(region['begin'],16), int(region['end'],16)
            assert end-begin == 54*528 and (end <= 0x3fcc0000 or begin >= 0x3fce0000), 'queue intersects DAC banks'
            assert r['ring_heap_bytes'] == 0 and r['workspace_heap_bytes'] == 8144, 'wrong workspace/queue layout'
            row['verdict'] = 'SETUP' if row['setup'] == 'keyed' else 'PASS'
        except Exception as e:
            row.update(verdict='FAIL',detail=str(e))
        summary.append(row)
        print(f"{row['verdict']}: {mc} {row['setup']}: "+
              (f"{row['queue_pages']} pages, Wi-Fi minimum {row['wifi_min_free']} B" if 'queue_pages' in row else '')+
              (f"; {row['frames_played']} data, {row['dummy_steady']} steady dummies, bank {row['max_fill_cycles']} cycles"
               if 'frames_played' in row else '')+
              (f" at sample {row['max_fill_sample']}; first startup gap at data {row['dummy_startup_frame']}"
               if 'max_fill_sample' in row else '')+
              (f"; regions {row['queue_layout']['regions']}" if 'queue_layout' in row else '')+
              (f"; largest after Wi-Fi {row['wifi_largest_up']} B; " +
               ', '.join(f"{k} {row['layout'][k]}" for k in ('fast','workspace','encoder_tables'))
               if 'queue_sizing' in row else '')+
              (f"; requested {row['requested_bytes']} B, free {row['free_internal']} B, largest {row['largest_internal']} B"
               if 'requested_bytes' in row else '')+
              (f"; {row['detail']}" if 'detail' in row else ''),flush=True)
    return summary


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('output',type=pathlib.Path)
    p.add_argument('--seconds',type=int,default=20,choices=range(20,56),metavar='20..55')
    p.add_argument('--observe-seconds',type=int,default=5,choices=range(1,21),metavar='1..20')
    p.add_argument('--port',default='/dev/cu.usbmodem101')
    mode = p.add_mutually_exclusive_group()
    mode.add_argument('--trace',action='store_true',help='15 and 17 only, 10 seconds each, capture-only trace without interrupt observer')
    mode.add_argument('--page-sweep',action='store_true',help='17 only, Wi-Fi off, 46/50/54/58 pages, 10 seconds each, no trace or observer')
    mode.add_argument('--queue-gate',action='store_true',help='13/15/17 keyed-layout memory probes and clean 20-second Wi-Fi-up gates; no TX, observer or trace')
    p.add_argument('--plan',action='store_true',help='print commands without opening/resetting a port')
    a = p.parse_args()
    if a.plan:
        print('info (reset once)')
        for *_,command in commands(a.seconds,a.observe_seconds,a.trace,a.page_sweep,a.queue_gate): print(command)
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
        summary = queue_suite(send,firmware) if a.queue_gate else suite(send,firmware,a.seconds,a.observe_seconds,a.trace,a.page_sweep)
    (a.output/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    print('Records:',a.output)
    return int(any(r['verdict'] == 'FAIL' for r in summary))


if __name__ == '__main__': sys.exit(main())
