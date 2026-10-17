"""Host/optional QEMU guards, independent integer convolution and .20 decode."""
import argparse
import json
import pathlib
import subprocess
import sys
import tempfile
import zlib

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'host'))
import machine as m


def records(text):
    rows = [json.loads(l[1:]) for l in text.splitlines() if l.startswith('@{')]
    assert len(rows) == 37,len(rows)
    seen = set()
    for r in rows[:-1]:
        peak,seed,phase = r['peak'],r['seed'],r['phase']
        seen.add((peak,seed,phase))
        src = m.source((2026,7,0xffffffff)[seed])
        assert r['table'] == m.checksum(m.table(peak).tobytes()),r
        assert r['rows'] == m.checksum(m.descriptors(src,phase,1367).tobytes()),r
        assert r['check'] == m.checksum(m.shape(src,phase,1367,peak).tobytes()),r
    assert seen == {(p,s,a) for p in (16,128,508) for s in range(3) for a in range(4)}
    assert rows[-1] == dict(test='machine_end',prepares=2880,slices=17280,errors=0),rows[-1]
    print('sign windows: 2880 guarded prepares, 17280 guarded slices; all words match Python')


def native(folder):
    (folder/'esp_attr.h').write_text('#define IRAM_ATTR\n')
    exe = folder/'machine'
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-DMACHINE_HOST',f'-I{folder}',
                    f'-I{ROOT}/firmware/main',str(ROOT/'tests/machine.c'),
                    str(ROOT/'firmware/main/machine.c'),'-o',str(exe)],check=True)
    records(subprocess.check_output([str(exe)],text=True))
    return exe


def sign_mapper(folder):
    component = ROOT/'firmware/components/dvbs2'
    exe = folder/'sign_mapper'
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-DSIGN_HOST',
                    f'-I{component}/include',str(ROOT/'tests/sign_mapper.c'),
                    str(component/'fast.c'),str(component/'dvbs2.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)


def loopback(exe,out,receiver):
    import decode
    import dvbs2
    import onair
    mc, frames, rate = 11,16,'9/10'
    data = np.frombuffer(onair.payload(mc,frames,'N0CALL'),np.uint8).reshape(frames,-1)
    phases = []
    for payload in data:
        bb = dvbs2.bbframe(np.unpackbits(payload),rate,ro=.20)
        phases.append(dvbs2.plframe_phases(dvbs2.fecframe(bb,rate),rate))
    phases = np.concatenate(phases)
    # Six history symbols are enough. Match moon-fill's padded final block,
    # preserving its already pinned waveform CRC rather than inventing one.
    c = np.r_[np.zeros(6,np.uint8),phases//2,np.zeros((-len(phases))%5,np.uint8)]
    c = np.pad(c,(0,(-len(c))%4))
    packed = np.sum(c.reshape(-1,4) << np.array([6,4,2,0]),axis=1).astype(np.uint8)
    out.mkdir(parents=True,exist_ok=True)
    src, words = out/'frame.quads',out/'frame.words'
    packed.tofile(src)
    symbols = 6+((len(phases)+4)//5)*5
    subprocess.run([str(exe),str(src),str(words),str(symbols)],check=True)
    got = np.fromfile(words,dtype='<u4')
    want = m.shape(packed.tobytes(),0,(symbols-6)//5)
    assert np.array_equal(got,want)
    i = ((got&1023).astype(np.int16)^512)-512
    q = (((got>>10)&1023).astype(np.int16)^512)-512
    iq = np.column_stack([i,q]).astype('<i2')
    crc = zlib.crc32(iq.tobytes())
    assert crc == 2575466360,crc  # exp/moon-wild-a f704aa0, independently checked full-axis convolution
    iq.tofile(out/'frame.iq')
    if not receiver:
        print('16-frame fractional stream: every C word matches Python; full-axis CRC 2575466360')
        return
    cf = out/'frame.cfile'
    z = (i+1j*q)/512
    np.r_[z,np.zeros(4096,complex)].astype('<c8').tofile(cf)
    record,payload = decode.decode(cf,sample_rate=80e6,symbol_rate=100e6/3,alpha=.20,
                                  receiver=receiver,log=out/'decode.log')
    at = onair.stream_match(data.tobytes(),payload,data.shape[1])
    assert at is not None and len(payload)//data.shape[1] >= 14,record
    assert all(record[k] == 0 for k in ('ldpc_checks_before','ldpc_checks_after','ldpc_bit_flips',
               'bch_failed_frames','bch_corrected_bits','bbheader_rejected_frames')),record
    record.update(iq_crc32=crc,contiguous_frames=len(payload)//data.shape[1],
                  first_frame=at//data.shape[1],payload_bit_errors=0,receiver_flush_samples=4096)
    (out/'decode.json').write_text(json.dumps(record,sort_keys=True)+'\n')
    print(json.dumps(record,sort_keys=True))


def storage_and_capacity():
    # 16-aligned pages include their own readable trailing source-word guard.
    used = 24576+54*528+4096+8144
    assert used == 65328 and used <= 65536
    print('65328 B store: sign tables, 54 guarded pages, LUT, workspace; no packed frame ring')



if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--qemu',type=pathlib.Path)
    p.add_argument('--out',type=pathlib.Path)
    p.add_argument('--receiver')
    a = p.parse_args()
    storage_and_capacity()
    with tempfile.TemporaryDirectory() as tmp:
        folder = pathlib.Path(tmp)
        exe = native(folder)
        sign_mapper(folder)
        loopback(exe,a.out or folder/'decode',a.receiver)
    if a.qemu: records(a.qemu.read_text())
