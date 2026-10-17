"""New rate lifecycle and every output sample against the independent Python encoder/FIR.

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
import rational
from test_stream import compile_stream


def test(output=None, receiver=None):
    assert (ROOT/'firmware/main/shaper_rational_taps.h').read_text() == rational.header()
    for num,den,span,alpha in ((12,5,12,.2),(3,1,9,.35)):
        margins = rational.mask(rational.taps(num,den,span,alpha),num,den,alpha)
        assert min(margins.values()) > .1, margins
        print('pulse-only mask with alias power and DAC hold:', num, den, margins)
    with tempfile.TemporaryDirectory() as tmp:
        folder = pathlib.Path(tmp)
        exe = compile_stream(folder)
        out = output or folder
        out.mkdir(parents=True, exist_ok=True)
        crcs = {}
        for mc,num,den,span,alpha in ((11,12,5,12,.2),(17,12,5,12,.2),(17,3,1,9,.35),
                                     (4,12,5,12,.2),(13,12,5,12,.2),(13,3,1,9,.35)):
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
            assert r['pulse_symbols'] == span and r['ring_slots'] == 3
            assert r['ring_format'] == 'packed' and r['packed_mapper'] == 'direct'
            assert r['workspace_memory'] == r['payload_memory'] == r['ring_memory'] == 'internal'
            assert r['ring_heap_bytes'] == 0 and r['workspace_heap_bytes'] == 12420
            assert not r['encoder_failed'] and r['underruns'] == r['overlap_mismatches'] == 0
            table = int(r['layout']['table'],16)
            slots = [int(p,16) for p in r['layout']['slots']]
            frame = ((onair.frame_symbols(mc)//2+31)//16)*16
            used = ((span*num*4+15)//16)*16
            assert slots == [table+used+i*frame for i in range(3)]
            filler = int(r['layout']['filler'],16)
            assert filler == table+used+3*frame
            window_bytes = 12+(16380*den+num-1)//num+2
            assert filler+1696+window_bytes <= table+65536
            assert r['bch_cache_bytes'] == (8192 if mc in (4,13) else 0)
            symbols = onair.symbols(mc, frames+2, callsign='N0CALL', rolloff=alpha)
            phases = np.rint(np.angle(symbols)*4/np.pi).astype(int)%8
            actual = np.fromfile(iq, dtype='<i2').reshape(-1,2)
            count = (frames*onair.frame_symbols(mc)*num+den-1)//den
            assert len(actual) == r['samples'] == count
            coeff = rational.taps(num,den,span,alpha)
            # Independent integer convolution covers each physical wrap and frame seam.
            for first in range(0,count,16380):
                expected = rational.shape(phases,first,min(16380,count-first),coeff,num,den)
                np.testing.assert_array_equal(actual[first:first+len(expected)], expected)
            crc = zlib.crc32(iq.read_bytes())
            assert crc == r['crc32']
            crcs[f'{mc}/{num}/80/16'] = crc
            print('stream',name,'CRC',crc,'all samples equal Python; allocation failures/cleanup pass')
            if receiver:
                command = [sys.executable,str(ROOT/'host/onair.py'),str(iq),'--rate','80000000',
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
        for mc,frames in ((17,256),(11,200)):
            subprocess.run([str(exe),str(mc),'12',str(folder/'wrap.iq'),'N0CALL','80000000',str(frames)],
                           capture_output=True,text=True,timeout=120,check=True)
        print('new modes: complete physical-bank wrap cycle, no leaks or sample discontinuities')
        golden = ROOT/'tests/data/rational_crcs.json'
        if golden.exists(): assert crcs == json.loads(golden.read_text())
        if output: (output/'crcs.json').write_text(json.dumps(crcs,indent=2)+'\n')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('output',nargs='?',type=pathlib.Path)
    p.add_argument('--receiver',type=pathlib.Path)
    a = p.parse_args()
    test(a.output,a.receiver)
