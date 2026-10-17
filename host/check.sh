#!/bin/sh
# Coordinator only. Never flashes or transmits. Moonshot paces start Wi-Fi RX.
# --plan opens no devices.
cd "$(dirname "$0")/.." || exit 1
exec "${PYTHON:-python3}" - "$@" <<'PY'
import argparse
import datetime as dt
import json
import os
import pathlib
import subprocess
import sys
import zlib

p = argparse.ArgumentParser(description='TX-off board gates, including Wi-Fi-up moonshot paces after build/flash.')
p.add_argument('out', nargs='?', type=pathlib.Path)
p.add_argument('--plan', action='store_true', help='print every criterion without running anything')
p.add_argument('--moonshot', action='store_true', help='33m333 and 8PSK 26m667 gates, plus slim regressions')
p.add_argument('--all-crcs', action='store_true', help='also dump every golden waveform on the board (much slower)')
p.add_argument('--encoder', action='store_true', help='encoder candidate: target 9/10, stage timings and estimated 10k-cycle input')
p.add_argument('--port', default='/dev/cu.usbmodem101')
a = p.parse_args()
if a.moonshot and a.encoder: p.error("choose --moonshot or --encoder")
python = sys.executable
receiver = os.getenv('RX', '/Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb')
firmware = subprocess.check_output(['git','describe','--always','--dirty'], text=True).strip()
golden = json.loads(pathlib.Path('tests/data/waveform_crcs.json').read_text())
# mc/sps/DAC MHz/frames. Original 57 CRCs: 46f90af with adopted opt-in pulses.
# The extra 17/4/80/16 fixture is checked by the plain encoder and LeanDVB.
selected = ['4/40/40/16', '13/8/40/16', '13/4/40/40',
            '4/4/80/16', '11/4/80/16', '13/4/80/16', '15/4/80/16',
            '4/3/80/16', '11/3/80/16']
main_selected = selected[:]
if a.moonshot:
    golden.update(json.loads(pathlib.Path('tests/data/moon_stream_crcs.json').read_text()))
    selected += ['11/12/80/16','17/12/80/16','17/3/80/16']
if a.all_crcs: selected = list(golden)
# Every modulation/rate path; 30 s for the four critical paths, 10 s for regressions.
# Sources are immutable 30 s board measurements; their cycle ceilings are never relaxed.
paces = [(4,'',10,'2026-10-17-next4','dvbpace 4 30'),
         (11,'5m',10,'2026-10-17-next5','dvbpace 11 5m 30'),
         (11,'10m',10,'2026-10-17-next5','dvbpace 11 10m 30'),
         (11,'20m',30,'2026-10-17-qpsk-goal','dvbpace 11 20mq4 30'),
         (11,'26m667',30,'2026-10-17-qpsk-goal','dvbpace 11 26m667 30'),
         (17,'5m',30,'2026-10-17-qpsk-goal','dvbpace 17 5m 30'),
         (13,'10m',30,'2026-10-17-next4','dvbpace 13 10m 30'),
         (15,'20m',30,'2026-10-17-next4','dvbpace 15 20ms6 30')]
main_paces = paces[:]
if a.encoder:
    selected = ['11/3/80/16','17/4/80/16','13/4/80/16','15/4/80/16',
                '13/8/40/16','4/40/40/16']
    paces = [(11,'26m667',30,'2026-10-17-qpsk-goal','dvbpace 11 26m667 30'),
             # 17 has no fast-stream board baseline. Reuse 15's fill ceiling;
             # the absolute post-payload target below is its encoder gate.
             (17,'20m',30,'2026-10-17-next4','dvbpace 15 20ms6 30'),
             (13,'20m',30,'2026-10-17-next4','dvbpace 13 20ms6 30'),
             (15,'20m',30,'2026-10-17-next4','dvbpace 15 20ms6 30'),
             (17,'5m',10,'2026-10-17-qpsk-goal','dvbpace 17 5m 30')]
selected = list(dict.fromkeys(main_selected+selected))
if a.all_crcs: selected = list(golden)
metrics = ('max_fill_cycles','encode_cycles_mean','encode_cycles_live_max')

def baseline(topic, command):
    rows = [json.loads(line) for f in sorted(pathlib.Path('results',topic).glob('*.jsonl'))
            for line in f.read_text().splitlines()]
    found = [r for r in rows if r.get('command') == command and r.get('status') == 'ESP_OK']
    assert len(found) == 1, (topic,command,len(found))
    return found[0]

main_limits = [{k:baseline(topic,cmd)[k]*102//100 for k in metrics} for _,_,_,topic,cmd in main_paces]
limits = [{k:baseline(topic,cmd)[k]*102//100 for k in metrics} for _,_,_,topic,cmd in paces]
if a.encoder: limits[1] = {'max_fill_cycles':limits[1]['max_fill_cycles']}
# Exact previously measured heap floors: free with Wi-Fi up / command low-water, bytes.
wifi = [(17,'5m',22351,22351), (11,'26m667',43203,41387), (13,'20m',36727,34875)]
if a.encoder: wifi = [(17,'5m',20480,20480), (11,'26m667',20480,20480), (17,'20m',20480,20480)]

main_wifi = wifi[:] if not a.encoder else [(17,'5m',22351,22351), (11,'26m667',43203,41387), (13,'20m',36727,34875)]
if a.moonshot:
    # Exact 240 MHz deadlines, 15% reserve. No host-clock timing thresholds.
    paces = [(mc,token,30,None,None) for mc,token in
                                  ((11,'33m333'),(17,'33m333'),(17,'26m667'))]
    limits = []
    for mc,token,_,_,_ in paces:
        num,den = (12,5) if token == '33m333' else (3,1)
        symbols = 33282 if mc < 12 else 22194
        ceiling = 240000000*symbols*num*85//(80000000*den*100)
        limits.append(dict(max_fill_cycles=40137, encode_cycles_mean=ceiling,
                           encode_cycles_live_max=ceiling))
    wifi = [(mc,token,20000,20000) for mc,token in
                                   ((11,'33m333'),(17,'33m333'),(17,'26m667'))]

# Baselines always run first, in this same session. Candidate checks add to them.
extra_paces = [(p,l) for p,l in zip(paces,limits)
               if (p[0],p[1]) not in {(b[0],b[1]) for b in main_paces}]
extra_wifi = [w for w in wifi if w[:2] not in {b[:2] for b in main_wifi}]
paces, limits = main_paces+[p for p,_ in extra_paces], main_limits+[l for _,l in extra_paces]
wifi = main_wifi+extra_wifi

def mode(mc, token):
    sps = {'':40,'5m':8,'10m':4,'20m':4,'26m667':3,'33m333':12}[token]
    rate = 80000000 if token in ('20m','26m667','33m333') else 40000000
    quads = mc < 12 and rate == 80000000
    return sps, rate, quads

def dump_args(key):
    mc,n,mhz,frames = map(int,key.split('/'))
    token = '33m333' if n == 12 else '26m667' if n == 3 else '20m' if mhz == 80 else {40:'',8:'5m',4:'10m'}[n]
    decim = 10 if n == 40 else 2 if mc < 12 and n == 8 else 1
    return mc,token,frames,decim

print(f'Pass criteria fixed before any command: firmware {firmware}, stored N0CALL; no NVS writes.')
print('Host: all 57 retained waveform CRCs plus the encoder fixture; every filled word equals the plain encoder.')
if a.moonshot: print('New modes: six split-kernel Python/C CRCs; fractional roll-off 0.20, 8PSK 3 sps 0.35; Wi-Fi-up paces; 40137 cycles/core/bank, 20000 B heap floor.')
print('FEC: 140 phase CRCs, 14 zero-mismatch PIE checks, all four stage bits active.')
print('Every stream: ESP_OK, encoder timed/healthy, zero underruns/stalls, correct rate/pulse/ring/mapper.')
print('Dumps: exact byte count, firmware/USB/file CRC equality, zero overlap errors; LeanDVB')
print('  recovers >=N-2 QPSK frames; 8PSK allows <=1 leading acquisition frame and <=3 EOF frames,')
print('  with zero LDPC checks/flips, BCH errors/corrections and payload errors.')
for key in selected: print('  dump', dump_args(key), 'CRC32', golden[key])
print('Paces: selected 30 s key runs and 10 s regressions; zero late fills/switches/fatal lateness, positive slack, live encoder, conserved fill words.')
print('Hard gate: every retained main dump and pace runs first; all three timing metrics <=102% of the fixed main measurements.')
print('A candidate is ineligible if any main gate fails, even when its own pace passes.')
print('Pace input: main modes use the main synthetic source; candidate copies are timed separately; all four PIE stage bits active.')
for (mc,token,seconds,topic,old),limit in zip(paces,limits):
    print(f'  {mc} {token or "1m"}, {seconds} s: ceilings {limit} cycles (encoder cycles/frame); '+(f'<=102% of {topic}: {old}' if topic else '85% of the exact deadline'))
for mc,token,up,low in wifi: print(f'  wifimem {mc} {token}: up >= {up} B, minimum >= {low} B; no playback.')
if a.moonshot:
    print('Core 0 capacity: helper bank maximum + 4792 scheduling + measured frame work (input included) * 47220/frame <= 40137 cycles.')
    print('  Dispatch <=2396 cycles/job; join wait is included in the full-bank core 1 maximum. This is a capacity screen, not a latency proof.')
if a.encoder:
    print('Encoder planning gate: same-frame post-payload mean AND live maximum + estimated 10,000 cycles/input copy')
    print('  <= 135,827 cycles/frame for internal 8PSK; <= 203,685 for QPSK 9/10 at 26m667.')
    print('  These are floor(0.85 * 240e6 * frame_symbols / (100e6/3)); real SPI input must be measured later.')
print('Serial ownership: lock across worktrees, reject cu/tty and reset-bridge owners before reset/open.')
print('Decode lookahead: append 4096 zero samples to new-mode receiver input only, outside the dump CRC.')
print('Continue after failures, print every result and exit nonzero if any gate fails.', flush=True)
if a.plan: sys.exit(0)
out = a.out or pathlib.Path('notes',dt.datetime.now().strftime('check-%Y%m%d-%H%M%S'))
out.mkdir(parents=True, exist_ok=False)
summary = []
lock = None
(out/'revision.json').write_text(json.dumps(dict(
    host_firmware=firmware, host_commit=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),
    port=a.port, started=dt.datetime.now(dt.timezone.utc).isoformat()),indent=2)+'\n')

def run(cmd, log, device=False):
    with (out/log).open('w') as f:
        result = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, timeout=240,
                                pass_fds=(lock.fileno(),) if device else ())
    assert result.returncode == 0, f'exit {result.returncode}; see {out/log}'

def step(label, fn):
    try:
        fn()
        verdict, detail = 'PASS', ''
    except Exception as e:
        verdict, detail = 'FAIL', str(e)
        if len(detail) > 500: detail = detail[:500] + ' ... (see runs.jsonl/logs)'
    summary.append((label,verdict,detail))
    with (out/'summary.tsv').open('a') as f: f.write(f'{verdict}\t{label}\t{detail}\n')
    print(verdict+': '+label+(' '+detail if detail else ''), flush=True)

def board(command, reset=False, save=None):
    cmd = [python,'host/board.py',command,'--port',a.port,'--timeout','120','--out',str(out/'runs.jsonl')]
    if reset: cmd += ['--reset']
    if save: cmd += ['--save',str(out/save)]
    run(cmd, command.replace(' ','-')+'.log', device=True)
    rows = [json.loads(l) for l in (out/'runs.jsonl').read_text().splitlines()]
    rows = [r for r in rows if r.get('command') == command]
    assert rows and not any('error' in r for r in rows), rows
    return rows

def info():
    rows = board('info', reset=True)
    (out/'board-info.json').write_text(json.dumps(rows,indent=2)+'\n')
    assert len(rows) == 1 and rows[0]['firmware'] == firmware and rows[0]['callsign'] == 'N0CALL', rows

def stream(rows, mc, token):
    results = [r for r in rows if 'status' in r]
    assert len(results) == 1, rows
    r = results[0]
    n,rate,quads = mode(mc,token)
    moon = n == 12 or (mc >= 12 and n == 3)
    den = 5 if n == 12 else 1
    resident = n != 40 and (moon or mc < 12 or n == 4)
    assert r['status'] == 'ESP_OK' and not r['encoder_failed'] and r['encoder_timed'], r
    assert r['callsign'] == 'N0CALL' and r['underruns'] == r['stall_ms'] == 0, r
    assert r['modcod'] == mc and r['sps'] == n/den and r['symbol_rate'] == rate*den//n, r
    assert r['rolloff'] == (.2 if n == 12 else .35), r
    if moon:
        assert r['sps_num'] == n and r['sps_den'] == den, r
        assert r['fill_kernel'] == 'moon-pie-split', r
    assert r['pulse_symbols'] == (7 if n == 12 else 6 if mc >= 12 and n in (3,4) and rate == 80000000 else 9), r
    assert r['ring_slots'] == (8 if n == 40 else 3 if n <= 4 or moon else 2), r
    assert r['ring_format'] == ('raw' if n == 40 else 'quads' if quads else 'packed'), r
    assert r['workspace_memory'] == ('internal' if resident else 'psram'), r
    assert r['packed_mapper'] == ('none' if n == 40 else 'quads' if quads else 'direct' if resident else 'sparse'), r
    return r

def dump(key):
    mc,token,frames,decim = dump_args(key)
    command = f'dvbdump {mc} {token} {frames} 128 {decim}'.replace('  ',' ')
    rows = board(command, save=key.replace('/','-')+'.iq')
    r = stream(rows,mc,token)
    binary = [b for b in rows if 'binary_bytes' in b]
    data = (out/(key.replace('/','-')+'.iq')).read_bytes()
    num,_,_ = mode(mc,token)
    den = 5 if token == '33m333' else 1
    samples = (frames*r['frame_symbols']*num+den-1)//den
    assert len(binary) == 1 and len(data) == binary[0]['binary_bytes'] == (samples+decim-1)//decim*4, r
    assert r['frames'] == frames and r['decimation'] == decim and r['overlap_mismatches'] == 0, r
    assert r['crc32'] == binary[0]['host_crc32'] == zlib.crc32(data) == golden[key], r

def decode(key):
    mc,token,frames,decim = dump_args(key)
    n,rate,_ = mode(mc,token)
    name = key.replace('/','-')
    source = out/(name+'.iq')
    if n == 12 or (mc >= 12 and n == 3):
        source = out/(name+'.tail.iq')
        source.write_bytes((out/(name+'.iq')).read_bytes()+bytes(4096*4))
    run([python,'host/onair.py',str(source),'--rate',str(rate//decim),
         '--symbol-rate',str(rate*(5 if n == 12 else 1)/n),'--rolloff',str(.2 if n == 12 else .35),'--scale','512','--stream',str(frames),'--modcod',str(mc),
         '--callsign','N0CALL','--receiver',receiver,'--shift','0'],name+'.decode.json')
    r = json.loads((out/(name+'.decode.json')).read_text())
    assert r['frame_lock'] and r['status_complete'] and r['payload_match'], r
    assert 0 <= r['first_frame'] <= 1, r
    assert (r['payload_frames'] + r['first_frame'] >= frames-3 if mc >= 12 else
            r['payload_frames'] >= frames-2), r
    assert all(r[k] == 0 for k in ('bch_failed_frames','bch_corrected_bits','ldpc_checks_before','ldpc_bit_flips','payload_bit_errors')), r

def pace(mc,token,seconds,limit):
    r = stream(board(f'dvbpace {mc} {token} {seconds}'.replace('  ',' ')),mc,token)
    assert r['seconds'] == seconds and r['dac_rate'] == mode(mc,token)[1], r
    assert all(r[k] == 0 for k in ('late_fills','late_switches','fatal_late')), r
    assert r['min_fill_slack_cycles'] > 0 and all(r[k] <= v for k,v in limit.items()), r
    assert r['payload_source'] == 'precomputed-ring' and r['input_frames'] == 8 and r['input_memory'] == 'psram', r
    assert r['encoder_pie'] == 15 and r['frames_encoded'] > r['prefill_frames'], r
    stages = r['profile']['encoder'].values()
    assert all(0 <= p['helper_cycles'] <= p['cycles'] for p in stages), r
    if a.encoder and (token == '20m' or mc == 11):
        ceiling = 203685 if mc < 12 else 135827
        assert r['encode_kernel_cycles_mean'] + 10000 <= ceiling, r
        assert r['encode_kernel_cycles_live_max'] + 10000 <= ceiling, r
    if token == '33m333' or (mc >= 12 and token == '26m667'):
        assert r['paced_wifi_up'] and r['wifi_free_up'] >= 20000 and r['wifi_min_free'] >= 20000, r
        assert r['helper_jobs'] > 0 and r['helper_bank_max_cycles'] > 0, r
        # Capacity screen, not a WCET proof. Helper includes packing and kernel;
        # 2,396 cycles/job covers scheduling, based on the prior notification max.
        # Two physical pieces can dispatch twice. Never count helper time as encoder work.
        n,rate,_ = mode(mc,token)
        den = 5 if n == 12 else 1
        frame = (33282 if mc < 12 else 22194)*240000000*n/(rate*den)
        encoder = max(r['encode_frame_work_cycles_mean'],r['encode_frame_work_cycles_live_max'])
        assert r['helper_bank_max_cycles']+4792+encoder*47220/frame <= 40137, r
        assert r['dispatch_max_cycles'] <= 2396, r
    assert r['prefill_frames'] == r['ring_slots'] and r['frames_played'] > 0, r
    f = r['profile']
    assert f['shaped_words'] + f['reused_words'] == f['fill_words'], r

def memory(mc,token,up,low):
    r = stream(board(f'wifimem {mc} {token}'),mc,token)
    assert r['wifi_free_up'] >= up and r['wifi_min_free'] >= low, r
    assert r['frames_played'] == r['positions'] == r['max_fill_call_cycles'] == 0, r

def fec():
    board('fecbench pie')
    run([python,'tests/test_fecbench.py',str(out/'runs.jsonl'),'--correctness-only'],'fec-check.log')

try:
    sys.path.insert(0,'host')
    import board as owner
    lock = owner.reserve_port(a.port, reset=True)
    os.environ['ESP_DVBS2_LOCK_FD'] = str(lock.fileno())
except Exception as e:
    step('serial ownership', lambda: (_ for _ in ()).throw(e))
    print((out/'summary.tsv').read_text(),end='')
    print('Board checks refused before reset/open. Records:',out)
    sys.exit(1)
step('all host waveform CRCs', lambda:run([python,'tests/test_stream.py'],'host-crcs.log'))
if a.moonshot: step('new host streams', lambda:run([python,'tests/test_rational.py'],'host-new.log'))
step('info',info)
step('fecbench pie',fec)
def check_dumps(keys):
    for key in keys:
        step('dump '+key,lambda key=key:dump(key))
        step('decode '+key,lambda key=key:decode(key))

def check_paces(runs, ceilings):
    for (mc,token,seconds,_,_),limit in zip(runs,ceilings):
        step(f'pace {mc} {token or "1m"}',lambda mc=mc,token=token,seconds=seconds,limit=limit:pace(mc,token,seconds,limit))

def check_memory(probes):
    for args in probes: step(f'wifimem {args[0]} {args[1]}',lambda args=args:memory(*args))

def require_passes():
    failed = [label for label,verdict,_ in summary if verdict == 'FAIL']
    assert not failed, 'ineligible: '+', '.join(failed)

check_dumps(main_selected)
check_paces(main_paces,main_limits)
check_memory(main_wifi)
step('main regression gate',require_passes)
check_dumps([key for key in selected if key not in main_selected])
check_paces([p for p,_ in extra_paces],[l for _,l in extra_paces])
check_memory(extra_wifi)
if a.moonshot or a.encoder: step('candidate admission',require_passes)
print('\nEvery result:')
print((out/'summary.tsv').read_text(),end='')
failed = sum(verdict == 'FAIL' for _,verdict,_ in summary)
print(f'{len(summary)-failed} passed, {failed} failed. Records: {out}')
sys.exit(bool(failed))
PY
