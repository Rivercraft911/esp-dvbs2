"""Fractional fill experiment, independent convolution, mask, C/PIE and symmetry. No devices."""
import json
import pathlib
import subprocess
import sys
import tempfile

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'host'))
import moon_fill as m
import shaper as s


def check_records(text):
    rows = [json.loads(x[1:]) for x in text.splitlines() if x.startswith('@{"test":"moon_fill"')]
    assert len(rows) == 324,len(rows)
    seen = set()
    for r in rows:
        name = tuple(m.MODES)[r['mode']]
        bits = m.MODES[name][4]
        block = 4 if r['mode'] == 0 else 5
        x, src = r['seed'], []
        for _ in range(1367):
            x ^= x << 13 & 0xffffffff
            x ^= x >> 17
            x ^= x << 5 & 0xffffffff
            src.append(x & ((1 << (bits*block))-1))
        src = np.array(src,np.uint16)
        code = (src[:,None] >> (bits*np.arange(block-1,-1,-1))) & ((1 << bits)-1)
        phase = code.ravel() if bits == 3 else 2*code.ravel()+1
        assert not r['errors'] and r['guards'],r
        assert r['table'] == s.checksum(np.concatenate([t.ravel() for t in m.table(name,r['peak'])])),r
        assert r['check'] == s.checksum(m.shape(name,phase[:(2+r['groups'])*block],r['peak'])),r
        seen.add((name,r['peak'],r['seed'],r['groups']))
    assert len(seen) == 324
    slices = [json.loads(x[1:]) for x in text.splitlines() if x.startswith('@{"test":"moon_slices"')]
    assert slices == [dict(test='moon_slices',cases=15552,errors=0)],slices
    quads = [json.loads(x[1:]) for x in text.splitlines() if x.startswith('@{"test":"moon_quads"')]
    assert quads == [dict(test='moon_quads',cases=20736,errors=0)],quads
    print('moon C/PIE: 324 unique cases, whole tables and every block match Python')


def test_model():
    assert m.HEADER.read_text() == m.header_text()
    rng = np.random.default_rng(2026)
    for name,(_,_,_,_,bits,groups) in m.MODES.items():
        block = 4 if name == '8psk3' else 5
        phase = rng.integers(0,1 << bits,(257+2)*block,dtype=np.uint8)
        if bits == 2: phase = 2*phase+1
        for peak in (16,128,508):
            tables = m.table(name,peak)
            r = m.parts(name,peak)
            assert (sum(x.min(1) for x in r) >= -peak).all()
            assert (sum(x.max(1) for x in r) <= peak).all()
            got, exact = s.iq(m.shape(name,phase,peak)),m.linear(name,phase,peak)
            error = max(abs(got.real-exact.real).max(),abs(got.imag-exact.imag).max())
            assert error <= len(groups)/2+1e-6,(name,peak,error)
            assert sum(x.nbytes for x in tables) == {'8psk3':73728,'qpsk24':98368,'8psk24':76800,'qpsk24c':27648}[name]
        margin = m.mask(name)
        assert min(margin[k] for k in ('upper_db','lower_db','far_db')) > 0,(name,margin)
        margin = m.mask(name,quantized=True)
        assert min(margin[k] for k in ('upper_db','lower_db','far_db')) > 0,(name,margin)
        # Independent linear alias formula and table covariance must agree before rounding.
        fn = np.linspace(0,1.7,257)
        a,b = m.table_power(name,fn,rounded=False),m.sampled_power(name,fn)
        np.testing.assert_allclose(a/a[0],b/b[0],atol=1e-9,rtol=2e-8)
        # Independent random sequence checks the covariance normalization and phase average.
        sample = rng.integers(0,1 << bits,block*32770,dtype=np.uint8)
        if bits == 2: sample = 2*sample+1
        power = np.mean(abs(s.iq(m.shape(name,sample)))**2)
        assert abs(power/m.covariance(name)[0].real-1) < .02,(name,power)
        assert m.symmetry(name) == sum(x.nbytes for x in m.table(name))//4
        assert m.half_symmetry(name) == sum(x.nbytes for x in m.table(name))//2
    print('moon model: rational convolution, bounds, quarter rotations, inverse-hold masks pass')


def test_fallback():
    # Check the rational geometry and compact-table account against independent
    # convolution, including the longer 108-sample block's internal phases.
    original = tuple(m.MODES)
    rows = list(m.fallback_report())
    assert tuple(m.MODES) == original
    assert [r['trimmed_table_bytes'] for r in rows] == [49664,139264,249856,641024]
    rng = np.random.default_rng(37)
    for r in rows:
        up,den = (5,2) if r['rolloff'] == .25 else (27,10)
        bits = 2 if r['mode'] == 'QPSK' else 3
        block, width = r['block_samples']*den//up, 4 if bits == 2 else 3
        positions = tuple(range(1-r['pulse_symbols'],block))
        groups = tuple(positions[j:j+width] for j in range(0,len(positions),width))
        name = f'fallback-{bits}-{up}'
        m.MODES[name] = (up,den,r['pulse_symbols'],r['rolloff'],bits,groups)
        try:
            phase = rng.integers(0,1 << bits,66*block,dtype=np.uint8)
            if bits == 2: phase = 2*phase+1
            actual,exact = s.iq(m.shape(name,phase)),m.linear(name,phase)
            assert max(abs(actual.real-exact.real).max(),abs(actual.imag-exact.imag).max()) <= len(groups)/2+1e-6
            fn = np.linspace(0,up/den,127)
            a,b = m.table_power(name,fn,rounded=False),m.sampled_power(name,fn)
            np.testing.assert_allclose(a/a[0],b/b[0],atol=1e-9,rtol=2e-8)
            assert min(r['mask_quantized'][k] for k in ('upper_db','lower_db','far_db')) > 0
            assert abs(r['symbol_rate']*(1+r['rolloff'])-40e6) < 1e-6
        finally: del m.MODES[name]
    print('fallback: four rational convolution/mask and compact-table accounts pass; timings unassembled')


def test_native():
    with tempfile.TemporaryDirectory() as tmp:
        folder = pathlib.Path(tmp)
        exe = m.build_native(folder)
        check_records(subprocess.check_output([str(exe)],text=True))


def test_bench():
    # Exercise the real console benchmark with counted heap failures and synthetic
    # clocks. This verifies cleanup and protocol only, never performance.
    with tempfile.TemporaryDirectory() as tmp:
        p = pathlib.Path(tmp)
        for h in ('esp_attr.h','esp_cpu.h','esp_heap_caps.h','radio_s3.h','sdkconfig.h'):
            (p/h).write_text('')
        (p/'env.h').write_text(r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#define IRAM_ATTR
#define DRAM_ATTR
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_SPIRAM 4
#define CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ 240
#define RADIO_S3_BANK_WORDS 16380
void *heap_caps_aligned_alloc(unsigned, size_t, unsigned);
void *heap_caps_malloc(size_t, unsigned);
void heap_caps_free(void *);
uint32_t esp_cpu_get_cycle_count(void);
uint32_t *radio_s3_bank(unsigned);
''')
        (p/'bench.c').write_text(r'''
#include <stdio.h>
#include <string.h>
#include "shaper.h"
#include "shaper_qpsk.h"
#include "shaper_moon.h"
#include "membench.h"
static int live, fail = -1;
void *heap_caps_aligned_alloc(unsigned a, size_t n, unsigned caps) {
    (void)caps;
    if (fail == 0) { fail = -1; return NULL; }
    if (fail > 0) --fail;
    void *v = NULL; assert(!posix_memalign(&v,a,n)); ++live; return v;
}
void *heap_caps_malloc(size_t n, unsigned caps) { return heap_caps_aligned_alloc(16,n,caps); }
void heap_caps_free(void *p) { if (p) { --live; free(p); } }
uint32_t esp_cpu_get_cycle_count(void) { static uint32_t t; return t += 17; }
uint32_t *radio_s3_bank(unsigned bank) { static uint32_t banks[2][16384]; return banks[bank]; }
void shaper_qpsk3_pie(uint32_t *d,const uint8_t *s,unsigned n,const uint32_t *t) { shaper_qpsk_c(d,s,n,t,3); }
void shaper_qpsk4_pie(uint32_t *d,const uint8_t *s,unsigned n,const uint32_t *t) { shaper_qpsk_c(d,s,n,t,4); }
void shaper_short4_pie(uint32_t *d,const uint8_t *s,int n,const uint32_t *t) {
    shaper_table_t table = {.blocks=(uint32_t *)t,.n=4,.bits=3,.parts=2,.block_words=8};
    shaper_fill_c(d,s,n,&table);
}
static void join(uint32_t *d,const uint16_t *s,unsigned n,const uint32_t *const *t,unsigned mode) {
    static uint32_t all[24672]; unsigned at = 0, parts = shaper_moon_parts(mode);
    for (unsigned j = 0; j < parts; ++j) {
        unsigned words = shaper_moon_part_words(mode,j);
        memcpy(all+at,t[j],4*words); at += words;
    }
    shaper_moon_c(d,s,n,all,mode);
}
void shaper_moon3_pie(uint32_t *d,const uint16_t *s,unsigned n,const uint32_t *a,const uint32_t *b,const uint32_t *c) {
    const uint32_t *t[] = {a,b,c}; join(d,s,n,t,0);
}
void shaper_moon24qc_pie(uint32_t *d,const uint16_t *s,unsigned n,const uint32_t *a,const uint32_t *b,const uint32_t *c) {
    const uint32_t *t[] = {a,b,c}; join(d,s,n,t,3);
}
void shaper_moon24q_pie(uint32_t *d,const uint16_t *s,unsigned n,const uint32_t *const *t) { join(d,s,n,t,1); }
void shaper_moon24p_pie(uint32_t *d,const uint16_t *s,unsigned n,const uint32_t *const *t) { join(d,s,n,t,2); }
int main(void) {
    membench_moon(); assert(live == 0);
    assert(freopen("/dev/null","w",stdout));
    for (int i = 0; i < 7; ++i) { fail = i; membench_moon(); assert(live == 0); }
    return 0;
}
''')
        exe = p/'bench'
        subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-include',str(p/'env.h'),
                        f'-I{p}',f'-I{ROOT}/firmware/main',str(p/'bench.c'),
                        *[str(ROOT/'firmware/main'/f) for f in ('membench.c','shaper.c','shaper_short.c','shaper_qpsk.c','shaper_moon.c')],
                        '-o',str(exe)],check=True)
        log = p/'bench.log'
        log.write_text(subprocess.check_output([str(exe)],text=True))
        m.check_membench(log)
    print('moon benchmark: real driver checks/hashes, all seven allocation failures clean up')


def test_plan():
    with tempfile.TemporaryDirectory() as tmp:
        out = pathlib.Path(tmp)/'uncreated'
        plan = subprocess.check_output([str(ROOT/'host/check_fill.sh'),'--plan',str(out)],text=True)
        assert not out.exists()
        assert '40137' in plan and '20000' in plan and '34137' in plan
        assert 'Admission FAIL' in plan and all(str(v) in plan for v in
                    json.loads((ROOT/'tests/data/moon_waveform_crcs.json').read_text()).values())
    print('moon plan: fixed CRC, timing and RAM gates; no output directory or device access')


if __name__ == '__main__':
    test_model()
    test_fallback()
    test_native()
    test_bench()
    test_plan()
    if len(sys.argv) > 1: check_records(pathlib.Path(sys.argv[1]).read_text())
