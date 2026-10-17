"""New rate lifecycle and every output sample against the independent Python encoder and block convolution.

No silicon timing or heap claims. Optional --receiver decodes actual C stream dumps.
"""
import argparse
import json
import pathlib
import subprocess
import sys
import tempfile
import zlib
import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'host'))
import onair
import moon_fill as moon
from test_stream import compile_stream


def test(output=None, receiver=None):
    with tempfile.TemporaryDirectory() as tmp:
        folder = pathlib.Path(tmp)
        exe = compile_stream(folder)
        out = output or folder
        out.mkdir(parents=True, exist_ok=True)
        crcs = {}
        for mc,num,den,span,alpha in ((11,12,5,7,.2),(4,12,5,7,.2)):
            frames = 16
            name = f'{mc}-{num}-{den}'
            iq = out/(name+'.iq')
            result = subprocess.run([str(exe),str(mc),str(num),str(iq),'N0CALL','80000000',str(frames)],
                                    capture_output=True,text=True,timeout=90)
            assert result.returncode == 0, (result.stdout,result.stderr)
            (out/(name+'.log')).write_text(result.stdout)
            records = [json.loads(l[1:]) for l in result.stdout.splitlines() if l.startswith('@')]
            dumps = [r for r in records if 'crc32' in r]
            assert len(dumps) == 2 and dumps[0]['crc32'] == dumps[1]['crc32']
            r = dumps[0]
            assert r['rolloff'] == alpha and r['sps'] == num/den
            assert r['sps_num'] == num and r['sps_den'] == den
            assert r['symbol_rate'] == 80000000*den//num
            assert r['pulse_symbols'] == span and r['ring_slots'] == 2
            quads = mc < 12
            assert r['ring_format'] == ('quads' if quads else 'packed')
            assert r['packed_mapper'] == ('quads' if quads else 'direct')
            assert r['fill_kernel'] == 'moon-queue' and r['prepare_pages'] > 0
            assert r['queue_empty'] == 0 and r['queue_low_pages'] > 0 and r['local_pages'] > 0
            assert r['workspace_memory'] == r['payload_memory'] == r['ring_memory'] == 'internal'
            assert r['workspace_heap_bytes'] == 12420
            assert not r['encoder_failed'] and r['underruns'] == r['overlap_mismatches'] == 0
            assert r['bch_cache_bytes'] == (8192 if mc in (4,13) else 0)
            kernel = 'qpsk24c' if quads else '8psk24' if num == 12 else '8psk3'
            assert r['table_bytes'] == sum(t.nbytes for t in moon.table(kernel))
            symbols = onair.symbols(mc, frames+2, callsign='N0CALL', rolloff=alpha)
            phases = np.rint(np.angle(symbols)*4/np.pi).astype(int)%8
            actual = np.fromfile(iq, dtype='<i2').reshape(-1,2)
            count = (frames*onair.frame_symbols(mc)*num+den-1)//den
            assert len(actual) == r['samples'] == count
            block = 5 if num == 12 else 4
            pad = (-len(phases))%block
            padded = np.r_[np.full(2*block,1 if quads else 0),phases,np.zeros(pad)].astype(np.uint8)
            if quads and pad: padded[-pad:] = 1
            expected = moon.shape(kernel,padded)[:count]
            import shaper
            z = shaper.iq(expected)
            np.testing.assert_array_equal(actual,np.column_stack((z.real,z.imag)))
            crc = zlib.crc32(iq.read_bytes())
            assert crc == r['crc32']
            crcs[f'{mc}/{num}/80/16'] = crc
            print('stream',name,'CRC',crc,'all samples equal Python; allocation failures/cleanup pass')
            if receiver:
                decode_iq = out/(name+'.tail.iq')
                decode_iq.write_bytes(iq.read_bytes()+bytes(4096*4))
                command = [sys.executable,str(ROOT/'host/onair.py'),str(decode_iq),'--rate','80000000',
                           '--symbol-rate',str(80000000*den/num),'--rolloff',str(alpha),
                           '--modcod',str(mc),'--scale','512','--stream','16','--callsign','N0CALL',
                           '--shift','0','--receiver',str(receiver)]
                result = subprocess.run(command,capture_output=True,text=True,timeout=90)
                (out/(name+'.decode.json')).write_text(result.stdout)
                assert result.returncode == 0, (command,result.stdout,result.stderr)
                decoded = json.loads(result.stdout)
                assert decoded['payload_match'] and decoded['frame_lock'] and decoded['status_complete']
                assert 0 <= decoded['first_frame'] <= 1
                assert (decoded['payload_frames']+decoded['first_frame'] >= frames-3 if mc >= 12 else
                        decoded['payload_frames'] >= frames-2), decoded
                assert all(decoded[k] == 0 for k in ('bch_failed_frames','bch_corrected_bits',
                           'ldpc_checks_before','ldpc_bit_flips','payload_bit_errors')), decoded
                print('LeanDVB',name,decoded['payload_frames'],'bit-exact frames')
        for mc,frames in ((11,200),):
            subprocess.run([str(exe),str(mc),'12',str(folder/'wrap.iq'),'N0CALL','80000000',str(frames)],
                           capture_output=True,text=True,timeout=120,check=True)
        print('new modes: complete physical-bank wrap cycle, no leaks or sample discontinuities')
        golden = ROOT/'tests/data/moon_stream_crcs.json'
        if golden.exists(): assert all(v == json.loads(golden.read_text())[k] for k,v in crcs.items())
        if output: (output/'crcs.json').write_text(json.dumps(crcs,indent=2)+'\n')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('output',nargs='?',type=pathlib.Path)
    p.add_argument('--receiver',type=pathlib.Path)
    a = p.parse_args()
    test(a.output,a.receiver)
