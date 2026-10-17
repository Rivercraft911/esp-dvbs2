#!/bin/sh
# Coordinator only. Never flashes or keys RF. --plan opens no devices.
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

p = argparse.ArgumentParser(description='Radio-off board gates, about five minutes after build/flash.')
p.add_argument('out', nargs='?', type=pathlib.Path)
p.add_argument('--plan', action='store_true', help='print every criterion without running anything')
p.add_argument('--all-crcs', action='store_true', help='also dump every golden waveform on the board (much slower)')
p.add_argument('--encoder', action='store_true', help='encoder candidate: target 9/10, stage timings and estimated 10k-cycle input')
a = p.parse_args()
python = sys.executable
receiver = os.getenv('RX', '/Users/river/Documents/stanford/SSI/esp-dvbs2/notes/leansdr/src/apps/leandvb')
firmware = subprocess.check_output(['git','describe','--always','--dirty'], text=True).strip()
golden = json.loads(pathlib.Path('tests/data/waveform_crcs.json').read_text())
# mc/sps/DAC MHz/frames. Golden CRCs come from 46f90af, including its adopted opt-in pulses.
selected = ['4/40/40/16', '13/8/40/16', '13/4/40/40',
            '4/4/80/16', '11/4/80/16', '13/4/80/16', '15/4/80/16',
            '4/3/80/16', '11/3/80/16']
# Every modulation/rate path; 30 s for the four critical paths, 10 s for regressions.
# Sources are immutable 30 s board measurements; their cycle ceilings are never relaxed.
paces = [(4,'',10,'2026-10-17-next4','dvbpace 4 30'),
         (11,'5m',10,'2026-10-17-next5','dvbpace 11 5m 30'),
         (11,'10m',10,'2026-10-17-next5','dvbpace 11 10m 30'),
         (11,'20m',30,'2026-10-17-qpsk-goal','dvbpace 11 20mq4 30'),
         (11,'26m667',30,'2026-10-17-qpsk-goal','dvbpace 11 26m667 30'),
         (17,'5m',30,'2026-10-17-qpsk-goal','dvbpace 17 5m 30'),
         (13,'10m',10,'2026-10-17-next4','dvbpace 13 10m 30'),
         (15,'20m',30,'2026-10-17-next4','dvbpace 15 20ms6 30')]
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
if a.all_crcs: selected = list(golden)
metrics = ('max_fill_cycles','encode_cycles_mean','encode_cycles_live_max')

def baseline(topic, command):
    rows = [json.loads(line) for f in sorted(pathlib.Path('results',topic).glob('*.jsonl'))
            for line in f.read_text().splitlines()]
    found = [r for r in rows if r.get('command') == command and r.get('status') == 'ESP_OK']
    assert len(found) == 1, (topic,command,len(found))
    return found[0]

limits = [{k:baseline(topic,cmd)[k]*102//100 for k in metrics} for _,_,_,topic,cmd in paces]
if a.encoder: limits[1] = {'max_fill_cycles':limits[1]['max_fill_cycles']}
# Exact previously measured heap floors: free with Wi-Fi up / command low-water, bytes.
wifi = [(17,'5m',22351,22351), (11,'26m667',43203,41387), (13,'20m',36727,34875)]
if a.encoder: wifi = [(17,'5m',20480,20480), (11,'26m667',20480,20480), (17,'20m',20480,20480)]

def mode(mc, token):
    sps = {'':40,'5m':8,'10m':4,'20m':4,'26m667':3}[token]
    rate = 80000000 if token in ('20m','26m667') else 40000000
    quads = mc < 12 and rate == 80000000
    return sps, rate, quads

def dump_args(key):
    mc,n,mhz,frames = map(int,key.split('/'))
    token = '26m667' if n == 3 else '20m' if mhz == 80 else {40:'',8:'5m',4:'10m'}[n]
    decim = 10 if n == 40 else 2 if mc < 12 and n == 8 else 1
    return mc,token,frames,decim

print(f'Pass criteria fixed before any command: firmware {firmware}, stored N0CALL; no NVS writes.')
print('Host: all',len(golden),'waveform CRCs; every filled word equals the plain encoder.')
print('FEC: 140 phase CRCs, 14 zero-mismatch PIE checks, all four stage bits active.')
print('Every stream: ESP_OK, encoder timed/healthy, zero underruns/stalls, correct rate/pulse/ring/mapper.')
print('Dumps: exact byte count, firmware/USB/file CRC equality, zero overlap errors; LeanDVB')
print('  recovers >=N-2 QPSK frames; 8PSK allows <=1 leading acquisition frame and <=3 EOF frames,')
print('  with zero LDPC checks/flips, BCH errors/corrections and payload errors.')
for key in selected: print('  dump', dump_args(key), 'CRC32', golden[key])
print('Paces: zero late fills/switches/fatal lateness, positive slack, live encoder, conserved fill words.')
for (mc,token,seconds,topic,old),limit in zip(paces,limits):
    print(f'  {mc} {token or "1m"}, {seconds} s: ceilings {limit} cycles (encoder cycles/frame), <=102% of {topic}: {old}')
for mc,token,up,low in wifi: print(f'  wifimem {mc} {token}: up >= {up} B, minimum >= {low} B; no playback.')
if a.encoder:
    print('Encoder planning gate: same-frame post-payload mean AND live maximum + estimated 10,000 cycles/input copy')
    print('  <= 135,827 cycles/frame for internal 8PSK; <= 203,685 for QPSK 9/10 at 26m667.')
    print('  These are floor(0.85 * 240e6 * frame_symbols / (100e6/3)); real SPI input must be measured later.')
print('Continue after failures, print every result and exit nonzero if any gate fails.', flush=True)
if a.plan: sys.exit(0)
out = a.out or pathlib.Path('notes',dt.datetime.now().strftime('check-%Y%m%d-%H%M%S'))
out.mkdir(parents=True, exist_ok=False)
summary = []

def run(cmd, log):
    with (out/log).open('w') as f:
        result = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, timeout=240)
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
    cmd = [python,'host/board.py',command,'--timeout','120','--out',str(out/'runs.jsonl')]
    if reset: cmd += ['--reset']
    if save: cmd += ['--save',str(out/save)]
    run(cmd, command.replace(' ','-')+'.log')
    rows = [json.loads(l) for l in (out/'runs.jsonl').read_text().splitlines()]
    rows = [r for r in rows if r.get('command') == command]
    assert rows and not any('error' in r for r in rows), rows
    return rows

def info():
    rows = board('info', reset=True)
    assert len(rows) == 1 and rows[0]['firmware'] == firmware and rows[0]['callsign'] == 'N0CALL', rows

def stream(rows, mc, token):
    results = [r for r in rows if 'status' in r]
    assert len(results) == 1, rows
    r = results[0]
    n,rate,quads = mode(mc,token)
    resident = n != 40 and (mc < 12 or n == 4)
    assert r['status'] == 'ESP_OK' and not r['encoder_failed'] and r['encoder_timed'], r
    assert r['callsign'] == 'N0CALL' and r['underruns'] == r['stall_ms'] == 0, r
    assert r['modcod'] == mc and r['sps'] == n and r['symbol_rate'] == rate//n, r
    assert r['pulse_symbols'] == (6 if mc >= 12 and rate == 80000000 else 9), r
    assert r['ring_slots'] == (8 if n == 40 else 3 if n <= 4 else 2), r
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
    assert len(binary) == 1 and len(data) == binary[0]['binary_bytes'] == frames*r['frame_symbols']*r['sps']//decim*4, r
    assert r['frames'] == frames and r['decimation'] == decim and r['overlap_mismatches'] == 0, r
    assert r['crc32'] == binary[0]['host_crc32'] == zlib.crc32(data) == golden[key], r

def decode(key):
    mc,token,frames,decim = dump_args(key)
    n,rate,_ = mode(mc,token)
    name = key.replace('/','-')
    run([python,'host/onair.py',str(out/(name+'.iq')),'--rate',str(rate//decim),
         '--symbol-rate',str(rate/n),'--scale','512','--stream',str(frames),'--modcod',str(mc),
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
    if a.encoder and (token == '20m' or mc == 11):
        ceiling = 203685 if mc < 12 else 135827
        assert r['encode_kernel_cycles_mean'] + 10000 <= ceiling, r
        assert r['encode_kernel_cycles_live_max'] + 10000 <= ceiling, r
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

step('all host waveform CRCs', lambda:run([python,'tests/test_stream.py'],'host-crcs.log'))
step('info',info)
step('fecbench pie',fec)
for key in selected:
    step('dump '+key,lambda key=key:dump(key))
    step('decode '+key,lambda key=key:decode(key))
for (mc,token,seconds,_,_),limit in zip(paces,limits):
    step(f'pace {mc} {token or "1m"}',lambda mc=mc,token=token,seconds=seconds,limit=limit:pace(mc,token,seconds,limit))
for args in wifi: step(f'wifimem {args[0]} {args[1]}',lambda args=args:memory(*args))
print('\nEvery result:')
print((out/'summary.tsv').read_text(),end='')
failed = sum(verdict == 'FAIL' for _,verdict,_ in summary)
print(f'{len(summary)-failed} passed, {failed} failed. Records: {out}')
sys.exit(bool(failed))
PY
