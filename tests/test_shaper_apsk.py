"""Host-only checks for the isolated APSK prototypes. No board/SDR access.

python3 tests/test_shaper_apsk.py
python3 tests/test_shaper_apsk.py --fixture notes/apsk-tools/app/main/fixture.h
"""
import ctypes as ct
import pathlib
import shutil
import subprocess
import sys
import tempfile
import numpy as np

ROOT=pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'host'))
import shaper_apsk as a
import shaper as s


def profiles():
    for m,rates in [(16,a.GAMMA16),(32,a.GAMMA32)]:
        for rate in rates:
            for n in [4,8]:
                for frac in [0,3]:
                    yield a.Profile(m=m,rate=rate,n=n,frac=frac)


def test_constellations():
    # Independent first-quadrant label->(ring, angle) readings of figures 11/12.
    c16={12:(0,45),4:(1,15),0:(1,45),8:(1,75)}
    c32={17:(0,45),16:(1,15),0:(1,45),1:(1,75),24:(2,0),8:(2,22.5),25:(2,45),9:(2,67.5),13:(2,90)}
    for m,rates,checks in [(16,a.GAMMA16,c16),(32,a.GAMMA32,c32)]:
        for rate in rates:
            x=a.constellation(m,rate)
            assert abs(np.mean(abs(x[:m])**2)-1)<1e-14
            radii=np.unique(np.round(abs(x[:m]),12))
            expect=[rates[rate]] if m==16 else rates[rate]
            assert np.allclose(radii[1:]/radii[0],expect)
            for label,(ring,degrees) in checks.items():
                assert abs(x[label]/radii[ring]-np.exp(1j*np.deg2rad(degrees)))<1e-10
            # PL rotations map the data alphabet to itself, including axis points.
            assert max(min(abs(1j*v-x[:m])) for v in x[:m])<1e-14
            assert np.allclose(x[m:],np.exp(1j*(np.pi/4+np.arange(4)*np.pi/2)))
    print('constellations: figures 11/12, all gamma rows, unit energy, PL rotations and QPSK')


def direct(p,sym,pair=False):
    out=[]
    for k in range(8,len(sym)):
        v=np.zeros((p.n,2),np.int64)
        for group in range(5 if pair else 9):
            ages=range(2*group,min(2*group+2,9)) if pair else [group]
            acc=sum(p.levels[sym[k-age]]*p.taps[age*p.n:(age+1)*p.n,None] for age in ages)
            v+=a.rounded(acc,30-p.frac)
        if p.frac:
            v=a.rounded(v,p.frac)
        out.extend((v[:,0]&1023)|((v[:,1]&1023)<<10))
    return np.array(out,np.uint32)


def test_models():
    for p in profiles():
        for method in ['single','pair']:
            sym=a.cyclic(p,31)
            if method=='single':
                sym[8:16]=p.m+np.arange(8)%4  # exact transitions to/from QPSK
            got=a.shape(p,sym,method)
            assert np.array_equal(got,direct(p,sym,method=='pair')), (p,method)
            e=s.iq(got)-a.linear(p,sym)
            groups=9 if method=='single' else 5
            bound=groups*.5/(2**p.frac)+(.5 if p.frac else 0)
            assert max(abs(e.real).max(),abs(e.imag).max())<=bound+1e-9
            assert not (got>>20).any()
            # All repeated points exercise coherent peaks; data-only zero count is legal.
            assert len(a.shape(p,sym[:8],method if method=='single' else 'single'))==0
            for code in range(p.m+4 if method=='single' else p.m):
                v=np.full(17,code,np.uint8)
                assert np.array_equal(a.shape(p,v,method),direct(p,v,method=='pair'))
    print('model: direct convolution with group rounding, all 44 profiles and every constant point')


def test_c():
    cc=shutil.which('cc')
    assert cc, 'C compiler required'
    with tempfile.TemporaryDirectory() as tmp:
        tmp=pathlib.Path(tmp)
        (tmp/'esp_attr.h').write_text('#define IRAM_ATTR\n')
        lib=tmp/'apsk.so'
        subprocess.run([cc,'-std=c11','-O2','-shared','-fPIC','-Wall','-Wextra','-Werror',
                        '-I'+str(tmp),'-I'+str(ROOT/'firmware/main'),str(ROOT/'firmware/main/shaper_apsk.c'),
                        '-o',str(lib)],check=True)
        dll=ct.CDLL(str(lib))
        ptr=lambda x: ct.c_void_p(x.ctypes.data)
        for p in profiles():
            taps=p.taps.astype(np.int32); lev=np.ascontiguousarray(p.levels,dtype=np.int16)
            tab=np.empty_like(p.single)
            dll.apsk_build_single(ptr(tab),ptr(taps),ptr(lev),p.m,p.n,p.frac)
            assert np.array_equal(tab,p.single),(p,'single table')
            pair=np.empty_like(p.pair[0]); tail=np.empty_like(p.pair[1])
            dll.apsk_build_pair(ptr(pair),ptr(tail),ptr(taps),ptr(lev),p.m,p.n,p.frac)
            assert np.array_equal(pair,p.pair[0]) and np.array_equal(tail,p.pair[1]),(p,'pair table')
            for method in ['single','pair']:
                sym=a.cyclic(p,71)
                if method=='single': sym[15:51]=p.m+np.arange(36)%4
                expected=a.shape(p,sym,method)
                out=np.full(len(expected)+8,0xfeedface,np.uint32)
                args=[ptr(out[4:]),ptr(sym[8:]),ct.c_size_t(71)]
                if method=='single': dll.apsk_single_c(*args,ptr(tab),p.n,p.frac)
                else: dll.apsk_pair_c(*args,ptr(pair),ptr(tail),p.m,p.n,p.frac)
                assert np.array_equal(out[4:-4],expected),(p,method)
                assert (out[:4]==0xfeedface).all() and (out[-4:]==0xfeedface).all()
    print('C: every table word and output word equals Python for 44 profiles, guards intact')


def test_quality():
    for p in profiles():
        if not p.frac: continue
        for method in ['single','pair']:
            r=a.metrics(p,method,32768)
            assert r['mer_db']>34,(p,method,r)
            assert r['mask_linear_db']>.17,(p,method,r)
            assert r['mask_total_estimate_db']>.10,(p,method,r)
            assert r['rounding_floor_db'] < -42,(p,method,r)
            # Exact whole-count ensemble PSD has the same average power as a long IID run.
            if p.rate=='3/4':
                q=a.Profile(m=p.m,n=p.n)
                sym=a.cyclic(q,65536)
                psd=a.spectrum(q,method)
                power=np.mean(abs(s.iq(a.shape(q,sym,method)))**2)
                assert abs(psd.mean()/power-1)<.025
    print('signal model: all gamma rows, Q3 MER >34 dB and estimated mask >0.10 dB; PSD power check')


def fixture(path):
    """Small source fixture; model hashes let QEMU also verify its C builders."""
    def arr(v): return '{'+','.join(str(int(x)) for x in np.asarray(v).ravel())+'}'
    lines=['// Host-generated test inputs, not firmware tables.',
           'struct fixture { int m,n,frac; int32_t taps[72]; int16_t levels[36][2]; uint32_t single,pair,tail; };',
           'static const struct fixture fixtures[] = {']
    for p in profiles():
        lines.append('{'+f'{p.m},{p.n},{p.frac},'+arr(p.taps)+','+
                     '{'+','.join(arr(v) for v in p.levels)+'},'+
                     ','.join(str(s.checksum(t.ravel()))+'u' for t in [p.single,*p.pair])+'},')
    lines.append('};\n')
    pathlib.Path(path).write_text('\n'.join(lines))


def qemu_project(path):
    """Create an isolated ESP-IDF correctness app; never flash it."""
    dest=pathlib.Path(path).resolve();(dest/'main').mkdir(parents=True,exist_ok=True)
    (dest/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.16)\n'
        'include($ENV{IDF_PATH}/tools/cmake/project.cmake)\nproject(apsk_qemu)\n')
    main=ROOT/'firmware/main'
    (dest/'main/CMakeLists.txt').write_text('idf_component_register(SRCS '+
        ' '.join('"'+str(x)+'"' for x in [ROOT/'tests/shaper_apsk_qemu.c',main/'shaper_apsk.c',main/'shaper_apsk_pie.S'])+
        ' INCLUDE_DIRS "'+str(main)+'" ".")\n')
    (dest/'sdkconfig.defaults').write_text('CONFIG_IDF_TARGET="esp32s3"\n'
        'CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y\nCONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y\n'
        'CONFIG_COMPILER_OPTIMIZATION_PERF=y\nCONFIG_ESP_TASK_WDT_EN=n\nCONFIG_ESP_INT_WDT=n\n')
    fixture(dest/'main/fixture.h')


def test_cycle_rules():
    import shaper_apsk_cycles as c
    assert c.cycles(['ee.vld.128.ip q0, a6, 16','ee.xorq q1, q0, q7'])==3
    assert c.cycles(['ee.vld.128.ip q0, a6, 16','ssai 3','ee.xorq q1, q0, q7'])==3
    assert c.cycles(['addi a3, a3, 1','bnez a4, 100'])==4
    print('cycle model: load-use interlocks, scheduled gap and taken-branch penalty')


if __name__=='__main__':
    if len(sys.argv)==3 and sys.argv[1]=='--fixture':
        fixture(sys.argv[2])
    elif len(sys.argv)==3 and sys.argv[1]=='--qemu-project':
        qemu_project(sys.argv[2])
    else:
        for name,fn in list(globals().items()):
            if name.startswith('test_'): fn()
