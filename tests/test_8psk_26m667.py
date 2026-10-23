"""Actual transmitter lifecycle, all new words and independent receiver. No board."""
import argparse
import json
import pathlib
import os
import subprocess
import sys
import tempfile
import zlib
import numpy as np
from test_stream import compile_stream

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'host'))
sys.path.insert(0,str(ROOT/'tests/reference'))
import psk8_26m667 as lofty
import waveforms as onair
import decode


def checksum(data):
    h = 2166136261
    for b in data: h = ((h^b)*16777619)&0xffffffff
    return h


def kernels(text):
    rows = [json.loads(l[1:]) for l in text.splitlines() if l.startswith('@{')]
    assert len(rows) == 11,len(rows)
    assert "machine add: 4095 blocks" in text
    assert "lofty pages: 99 guarded batches" in text
    for r in rows[:9]:
        assert r['table'] == checksum(lofty.table(r['peak']).tobytes()),r
        x = (2026,7,0xffffffff)[r['seed']]
        words = []
        for _ in range(1380):
            x ^= (x<<13)&0xffffffff; x ^= x>>17; x ^= (x<<5)&0xffffffff
            words.append(x&4095)
        phases = ((np.array(words)[:,None] >> np.array([9,6,3,0]))&7).ravel()
        samples = lofty.shape(phases,r['peak'])[12:12+1367*12]
        assert r['samples'] == checksum(samples.tobytes()),r
    assert rows[9] == dict(test='lofty_end',slices=4320,errors=0)
    assert rows[10] == dict(test='lofty_mapper',frames=36,errors=0)
    print('4320 guarded slices, 36 mapper frames, all hashes equal independent Python')


def check(out, receiver=None):
    out.mkdir(parents=True,exist_ok=True)
    exe = compile_stream(out)
    component = ROOT/'firmware/components/dvbs2'
    kernel = out/'kernels'
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',
                    '-I'+str(out),'-I'+str(ROOT/'firmware/components/shaper/include'),'-I'+str(component/'include'),
                    str(ROOT/'tests/psk8_kernels.c'),
                    str(ROOT/'firmware/components/shaper/machine.c'),str(ROOT/'firmware/components/shaper/lofty.c'),
                    str(component/'fast.c'),str(component/'dvbs2.c'),'-o',str(kernel)],check=True)
    kernels(subprocess.check_output([str(kernel)],text=True))
    crcs = {}
    for mc in (13,15,17):
        iq = out/f'{mc}.iq'
        run = subprocess.run([str(exe),str(mc),'3',str(iq),'N0CALL','80000000','16'],
                             capture_output=True,text=True,timeout=90)
        (out/f'{mc}.log').write_text(run.stdout+run.stderr)
        assert run.returncode == 0, run.stderr
        rows = [json.loads(l[1:]) for l in run.stdout.splitlines() if l.startswith('@{')]
        keyed, = [r for r in rows if r.get('cmd') == 'dvb']
        assert keyed['prepared_fixture'] and keyed['payload_source'] == 'psram-frame'
        assert keyed['queue_pages'] == 54 and not keyed['encoder_failed'] and keyed['status'] == 'ESP_OK'
        dumps = [r for r in rows if 'crc32' in r]
        assert len(dumps) == 2 and dumps[0]['crc32'] == dumps[1]['crc32']
        r = dumps[0]
        assert r['profile']['fill_phase_cycles'] and r['profile']['fill']['whole']['calls'] > 0
        assert r['fill_kernel'] == 'lofty-queue' and r['ring_format'] == 'words'
        assert r['warmup_frames'] == 3 and r['bch_variant'] == ('resident-pair' if mc == 17 else 'split-pair')
        assert r['queue_wake_policy'] == 'bank' and r['queue_wakes'] > 0
        assert r['bch_pair_psram_bytes'] == (0 if mc == 17 else 4096)
        assert r['encoder_priority'] == 24
        assert r['dummy_startup'] == r['dummy_steady'] == 0
        assert r['admission_pages'] == 9 and r['admission_wait_pages'] == 26
        assert r['dummy_startup_frames'] == 3
        assert r['queue_stream_page_max_cycles'] == 0 # host clocks are synthetic
        assert r['queue_stream_bound_misses'] == 0
        assert r['sps_num'] == 3 and r['sps_den'] == 1 and r['rolloff'] == .35
        assert r['pulse_symbols'] == 5 and r['table_bytes'] == 36864
        assert r['queue_pages'] == {13:54,15:50,17:46}[mc] and r['queue_empty'] == r['underruns'] == r['overlap_mismatches'] == 0
        symbols = onair.symbols(mc,17,callsign='N0CALL')
        phases = np.rint(np.angle(symbols)*4/np.pi).astype(int)%8
        count = 16*onair.frame_symbols(mc)
        expected = lofty.iq(lofty.shape(phases[:count]))
        actual = np.fromfile(iq,dtype='<i2').reshape(-1,2)
        np.testing.assert_array_equal(actual,np.column_stack((expected.real,expected.imag)))
        crc = zlib.crc32(iq.read_bytes())
        assert crc == r['crc32']
        crcs[f'{mc}/3/80/16'] = crc
        print('8PSK',mc,'all words, repeated command, allocation failures, CRC',crc,flush=True)
        if receiver:
            tail = out/f'{mc}.tail.iq'
            tail.write_bytes(iq.read_bytes()+bytes(4096*4))
            cmd = [sys.executable,str(ROOT/'host/decode.py'),str(tail),'--rate','80000000',
                   '--symbol-rate',str(80000000/3),'--rolloff','.35','--modcod',str(mc),
                   '--scale','512','--stream','16','--callsign','N0CALL',
                   '--receiver',str(receiver)]
            rx = subprocess.run(cmd,capture_output=True,text=True,timeout=120)
            (out/f'{mc}.decode.json').write_text(rx.stdout)
            assert rx.returncode == 0,(rx.stdout,rx.stderr)
            d = json.loads(rx.stdout)
            assert d['frame_lock'] and d['status_complete'] and d['payload_match']
            assert d['first_frame'] in (0,1) and d['payload_frames']+d['first_frame'] >= 13
            assert all(d[k] == 0 for k in ('bch_failed_frames','bch_corrected_bits',
                       'ldpc_checks_before','ldpc_bit_flips','payload_bit_errors')),d
            print('LeanDVB',mc,d['payload_frames'],'bit-exact frames',flush=True)
    structures = json.loads((ROOT/'tests/data/psk8_26m667_frames.json').read_text())
    assert crcs == {key: value['data_only_crc32'] for key,value in structures.items()}
    check_dump_layouts(out,exe,structures)
    for peak in (16,128,508):
        margins = lofty.mask_margins(lofty.taps(peak),3,.35)
        assert min(margins[k] for k in ('upper','lower','far')) > 0,margins
        print('pulse mask',peak,margins)
    quantized = lofty.quantized_mask()
    assert min(quantized.values()) > 0,quantized
    print('exact rounded-table mask at peak 128',quantized)


def check_dump_layouts(out, exe, structures):
    """Reproduce board dump structure without silicon clocks or saved board files."""
    golden = json.loads((ROOT/'tests/data/psk8_26m667_crcs.json').read_text())
    for mc in (13,15,17):
        key = f'{mc}/3/80/16'
        before = tuple(structures[key]['dummy_before'])
        symbols = onair.symbols(mc,16,dummy_before=before,callsign='N0CALL')
        phases = np.rint(np.angle(symbols)*4/np.pi).astype(int)%8
        model = lofty.iq(lofty.shape(phases))[:16*22194*3]
        model = np.column_stack((model.real,model.imag)).astype('<i2')
        assert zlib.crc32(model.tobytes()) == golden[key]
        env = dict(os.environ)
        if before:
            starts = sorted(set(before))
            assert all(before.count(f) == 2 for f in starts)
            periods = {b-a for a,b in zip(starts,starts[1:])}
            assert len(periods) == 1
            env.update(DUMMY_FROM_FRAME=str(starts[0]), DUMMY_FRAME_PERIOD=str(periods.pop()))
        iq = out/f'layout-{mc}.iq'
        run = subprocess.run([str(exe),str(mc),'3',str(iq),'N0CALL','80000000','16'],
                             env=env,capture_output=True,text=True,timeout=90)
        (out/f'layout-{mc}.log').write_text(run.stdout+run.stderr)
        assert run.returncode == 0,(run.stdout,run.stderr)
        actual = np.fromfile(iq,dtype='<i2').reshape(-1,2)
        np.testing.assert_array_equal(actual,model)
        rows = [json.loads(l[1:]) for l in run.stdout.splitlines() if l.startswith('@{')]
        dumps = [r for r in rows if 'crc32' in r]
        assert len(dumps) == 2 and all(r['crc32'] == golden[key] for r in dumps)
        assert all(r['queue_empty'] == r['underruns'] == r['overlap_mismatches'] == 0 for r in dumps)
        layout = decode.stream_layout(mc,16,before)
        assert layout['expected_payload_frames'] == {13:16,15:14,17:12}[mc]
        print(mc,'pinned data/dummy layout, every word exact; CRC',golden[key],flush=True)


def check_dummies(out, receiver=None):
    out.mkdir(parents=True,exist_ok=True)
    exe = compile_stream(out)
    for mc in (13,15,17):
        iq = out/f'{mc}.iq'
        env = dict(os.environ,FORCE_DUMMIES='1',BANK_SHIFT='1540')
        r = subprocess.run([str(exe),str(mc),'3',str(iq),'N0CALL','80000000','16'],
                           env=env,capture_output=True,text=True,timeout=90)
        (out/f'{mc}.log').write_text(r.stdout+r.stderr)
        assert r.returncode == 0,(mc,r.stderr)
        rows = [json.loads(l[1:]) for l in r.stdout.splitlines() if l.startswith('@{')]
        dump, = [row for row in rows if 'crc32' in row][:1]
        assert dump['dummy_startup'] == 4 and dump['dummy_steady'] == 2 and dump['dummy_alignment'] == 3
        assert dump['queue_empty'] == dump['underruns'] == dump['overlap_mismatches'] == 0
        s = onair.symbols(mc,17,dummy_before=(0,0,1,1,5,5),callsign='N0CALL')
        phases = np.rint(np.angle(s)*4/np.pi).astype(int)%8
        expected = lofty.iq(lofty.shape(phases[:16*22194]))
        actual = np.fromfile(iq,dtype='<i2').reshape(-1,2)
        np.testing.assert_array_equal(actual,np.column_stack((expected.real,expected.imag)))
        print(mc,'six dummies, every DAC word equals Python, startup=4 steady=2',flush=True)
        if receiver:
            cmd = [sys.executable,str(ROOT/'host/decode.py'),str(iq),'--rate','80000000',
                   '--symbol-rate',str(80000000/3),'--rolloff','.35','--modcod',str(mc),
                   '--scale','512','--stream','16','--callsign','N0CALL','--receiver',str(receiver)]
            run = subprocess.run(cmd,capture_output=True,text=True,timeout=120)
            (out/f'{mc}.decode.json').write_text(run.stdout+run.stderr)
            assert run.returncode == 0,(mc,run.stdout,run.stderr)
            d = json.loads(run.stdout)
            assert d['frame_lock'] and d['status_complete'] and d['payload_match']
            assert d['first_frame'] in (0,1) and d['payload_frames'] >= 12
            assert all(d[k] == 0 for k in ('bch_failed_frames','bch_corrected_bits',
                       'ldpc_checks_before','ldpc_bit_flips','payload_bit_errors')),d
            print(mc,'LeanDVB:',d['payload_frames'],'contiguous exact data frames',flush=True)



if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('output',nargs='?',type=pathlib.Path)
    p.add_argument('--receiver',type=pathlib.Path)
    a = p.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        out = (a.output or pathlib.Path(tmp)).resolve()
        check(out,a.receiver)
        check_dummies(out/"dummy",a.receiver)
