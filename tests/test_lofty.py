"""Actual transmitter lifecycle, all new words and independent receiver. No board."""
import argparse
import json
import pathlib
import subprocess
import sys
import tempfile
import zlib
import numpy as np
from test_stream import compile_stream

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'host'))
import lofty
sys.path.insert(0,str(ROOT/'tests/reference'))
import waveforms as onair


def checksum(data):
    h = 2166136261
    for b in data: h = ((h^b)*16777619)&0xffffffff
    return h


def kernels(text):
    rows = [json.loads(l[1:]) for l in text.splitlines() if l.startswith('@{')]
    jitter = [r['jitter'] for r in rows if r.get('test') == 'jitter_target']
    rows = [r for r in rows if r.get('test') != 'jitter_target']
    if jitter:
        assert len(jitter) == 2
        for r in jitter:
            assert r['worst_bank']['cycles'] == 12345 and r['worst_bank']['irqs'] >= 1
            assert r['worst_bank']['notify_calls'] == 1 and r['worst_bank']['notify_cycles'] == 321
            assert all(sum(p['calls'] for p in c) >= 1 for c in r['cores'])
            for core,c in enumerate(r['cores']):
                assert any(f'FROM_CPU_INTR{core}' in p['sources'] for p in c)
            assert sum(b['calls'] for b in r['banks_by_irq_count']) == 1
        print('real per-core ISR dispatch, original arguments and repeat teardown: ok')
    assert len(rows) == 11,len(rows)
    assert "sign mapper: 96 frames" in text and "machine add: 4095 blocks" in text
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
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-DLOFTY_HOST',
                    '-I'+str(out),'-I'+str(ROOT/'firmware/main'),'-I'+str(component/'include'),
                    str(ROOT/'tests/lofty.c'),str(ROOT/'tests/sign_mapper.c'),
                    str(ROOT/'firmware/main/machine.c'),str(ROOT/'firmware/main/lofty.c'),
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
        dumps = [r for r in rows if 'crc32' in r]
        assert len(dumps) == 2 and dumps[0]['crc32'] == dumps[1]['crc32']
        r = dumps[0]
        assert r['fill_kernel'] == 'lofty-queue' and r['ring_format'] == 'words'
        assert r['warmup_frames'] == 3 and r['bch_variant'] == ('resident-pair' if mc == 17 else 'split-pair')
        assert r['queue_wake_policy'] == 'poll' and r['queue_wakes'] == r['queue_wake_max_cycles'] == 0
        assert r['bch_pair_psram_bytes'] == (0 if mc == 17 else 4096)
        assert r['encoder_priority'] == 5
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
    pinned = ROOT/'tests/data/lofty_crcs.json'
    if pinned.exists(): assert crcs == json.loads(pinned.read_text())
    (out/'crcs.json').write_text(json.dumps(crcs,indent=2)+'\n')
    for peak in (16,128,508):
        margins = lofty.mask_margins(lofty.taps(peak),3,.35)
        assert min(margins[k] for k in ('upper','lower','far')) > 0,margins
        print('pulse mask',peak,margins)
    quantized = lofty.quantized_mask()
    assert min(quantized.values()) > 0,quantized
    print('exact rounded-table mask at peak 128',quantized)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('output',nargs='?',type=pathlib.Path)
    p.add_argument('--receiver',type=pathlib.Path)
    p.add_argument('--qemu',type=pathlib.Path)
    a = p.parse_args()
    if a.qemu: kernels(a.qemu.read_text())
    with tempfile.TemporaryDirectory() as tmp:
        check((a.output or pathlib.Path(tmp)).resolve(),a.receiver)
