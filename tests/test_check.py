"""Run the real coordinator check against fake board/receiver tools. No devices."""
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
FAKE = r'''
import argparse,json,os,pathlib,sys,zlib
p=argparse.ArgumentParser()
p.add_argument('command'); p.add_argument('--out',type=pathlib.Path); p.add_argument('--save',type=pathlib.Path)
p.add_argument('--timeout'); p.add_argument('--reset',action='store_true'); a=p.parse_args()
x=a.command.split(); kind=x[0]; bad=os.getenv('FAIL_GATES')
assert kind in ('info','fecbench','dvbdump','dvbpace','wifimem'), 'RF command in check!'
with pathlib.Path('commands.jsonl').open('a') as f:f.write(json.dumps(a.command)+'\n')
if bad and a.command == 'dvbpace 11 26m667 30':sys.exit(7)
if kind=='info': rows=[dict(firmware='test-fw',callsign='N0CALL')]
elif kind=='fecbench': rows=[dict(cmd='fecbench',fake=True)]
else:
 mc=int(x[1]); named=x[2] in ('5m','10m','20m','26m667'); token=x[2] if named else ''
 n={'':40,'5m':8,'10m':4,'20m':4,'26m667':3}[token]; rate=80000000 if token in ('20m','26m667') else 40000000
 quads=mc<12 and rate==80000000; resident=n!=40 and (mc<12 or n==4)
 symbols=33282 if mc<12 else 22194
 r=dict(status='ESP_OK',encoder_failed=False,encoder_timed=True,callsign='N0CALL',underruns=0,stall_ms=0,
        modcod=mc,sps=n,symbol_rate=rate//n,dac_rate=rate,
        pulse_symbols=6 if mc>=12 and rate==80000000 else 9,
        ring_slots=8 if n==40 else 3 if n<=4 else 2,ring_format='raw' if n==40 else 'quads' if quads else 'packed',
        workspace_memory='internal' if resident else 'psram',
        packed_mapper='none' if n==40 else 'quads' if quads else 'direct' if resident else 'sparse')
 if kind=='dvbdump':
  frames=int(x[3] if named else x[2]); decim=int(x[-1]); size=frames*symbols*n//decim*4
  key=f'{mc}/{n}/{rate//1000000}/{frames}'; crc=json.loads(pathlib.Path('tests/data/waveform_crcs.json').read_text())[key]
  # Forge only the final 32 bits; this tests the real file-size and CRC checker.
  zero=zlib.crc32(bytes(size)); basis={}
  for bit in range(32):
   value=zlib.crc32((1<<bit).to_bytes(4,'little')) ^ zlib.crc32(bytes(4)); mask=1<<bit
   while value:
    top=value.bit_length()-1
    if top not in basis:basis[top]=(value,mask);break
    v,m=basis[top];value^=v;mask^=m
  value=crc^zero; mask=0
  while value:
   v,m=basis[value.bit_length()-1];value^=v;mask^=m
  with a.save.open('wb') as f:f.truncate(size)
  with a.save.open('r+b') as f:f.seek(size-4);f.write(mask.to_bytes(4,'little'))
  if bad and key=='4/4/80/16':crc^=1
  r.update(frames=frames,frame_symbols=symbols,decimation=decim,overlap_mismatches=0,crc32=crc)
  rows=[dict(binary_bytes=size,host_crc32=crc),r]
 elif kind=='dvbpace':
  r.update(seconds=int(x[-1]),late_fills=0,late_switches=0,fatal_late=0,min_fill_slack_cycles=1,
           max_fill_cycles=1,encode_cycles_mean=1,encode_cycles_live_max=1,
           prefill_frames=r['ring_slots'],frames_played=10,profile=dict(shaped_words=8,reused_words=2,fill_words=10))
  # Exercise the exact two-percent boundary for each timing metric.
  old={'11 20m':('dvbpace 11 20mq4 30','max_fill_cycles'),
       '13 10m':('dvbpace 13 10m 30','encode_cycles_mean'),
       '11 5m':('dvbpace 11 5m 30','encode_cycles_live_max')}
  if bad and ' '.join(x[1:3]) in old:
   command,metric=old[' '.join(x[1:3])]
   records=[json.loads(l) for f in pathlib.Path('results').glob('*/*.jsonl') for l in f.read_text().splitlines()]
   b=next(b for b in records if b.get('command')==command and b.get('status')=='ESP_OK')
   r[metric]=b[metric]*102//100+1
  rows=[r]
 else:
  r.update(wifi_free_up=50000,wifi_min_free=50000,frames_played=0,positions=0,max_fill_call_cycles=0)
  if bad and mc==17:r['wifi_free_up']=22350
  if bad and mc==11:r['wifi_min_free']=41386
  rows=[r]
with a.out.open('a') as f:
 for r in rows:f.write(json.dumps(dict(command=a.command,**r))+'\n')
'''
RX = r'''
import json,os,sys
mc=int(sys.argv[sys.argv.index('--modcod')+1]); frames=int(sys.argv[sys.argv.index('--stream')+1])
print(json.dumps(dict(frame_lock=True,status_complete=True,first_frame=0,
 payload_match=True,payload_frames=frames-(3 if mc>=12 else 2),bch_failed_frames=0,
 bch_corrected_bits=0,ldpc_checks_before=0,ldpc_bit_flips=0,
 payload_bit_errors=int(bool(os.getenv('FAIL_GATES') and '11-3-80' in sys.argv[1])))))
'''
with tempfile.TemporaryDirectory() as tmp:
    root = pathlib.Path(tmp)
    for folder in ('host','tests/data','bin'):(root/folder).mkdir(parents=True)
    shutil.copy(ROOT/'host/check.sh',root/'host/check.sh')
    shutil.copy(ROOT/'tests/data/waveform_crcs.json',root/'tests/data/waveform_crcs.json')
    for topic in ('2026-10-17-qpsk-goal','2026-10-17-next4','2026-10-17-next5'):
        (root/'results'/topic).mkdir(parents=True)
        for file in (ROOT/'results'/topic).glob('*.jsonl'):shutil.copy(file,root/'results'/topic/file.name)
    (root/'host/board.py').write_text(FAKE)
    (root/'host/onair.py').write_text(RX)
    for name in ('test_stream.py','test_fecbench.py'):(root/'tests'/name).write_text('print("fake host check")\n')
    (root/'bin/git').write_text('#!/bin/sh\nprintf "test-fw\\n"\n')
    (root/'bin/git').chmod(0o755)
    env=dict(os.environ,PYTHON=sys.executable,PATH=str(root/'bin')+os.pathsep+os.environ['PATH'])
    plan=subprocess.run(['sh','host/check.sh','--plan'],cwd=root,env=env,capture_output=True,text=True,check=True)
    assert not (root/'commands.jsonl').exists() and '22351 B' in plan.stdout
    for fail in (False,True):
        out=root/('bad' if fail else 'good')
        run=subprocess.run(['sh','host/check.sh',str(out)],cwd=root,
                           env=dict(env,**({'FAIL_GATES':'1'} if fail else {})),capture_output=True,text=True,timeout=60)
        assert run.returncode == int(fail), (run.stdout,run.stderr)
        summary=(out/'summary.tsv').read_text().splitlines()
        assert len(summary)==32, summary
        failures=[line for line in summary if line.startswith('FAIL')]
        assert len(failures)==(8 if fail else 0), failures
        assert run.stdout.index('Pass criteria') < run.stdout.index('PASS:')
        commands=[json.loads(line) for line in (root/'commands.jsonl').read_text().splitlines()]
        assert commands[-1]=='wifimem 13 20m' and len(commands)==22*(2 if fail else 1), commands
print('check: dry plan, all commands run after failures, strict CRC/decode/2%/heap gates, final exit and summary pass')
