"""Independent waveform/table checks; optional QEMU or board records, never serial."""
import json
import pathlib
import subprocess
import sys
import tempfile
import numpy as np
ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'host'))
import round2 as m
import shaper as s


def check(rows):
    assert len(rows) == 162, len(rows)
    for r in rows:
        p = m.profile(r['peak'])
        assert r['mismatch'] == 0 and r['guards'] == 1, r
        assert r['table'] == s.checksum(p.table.ravel()), r
        sym = s.random_symbols(p, r['symbols']+8, r['seed'])
        assert r['check'] == s.checksum(s.shape(p, sym[3:])), r


def check_board(path):
    rows = [json.loads(l.removeprefix('@')) for l in path.read_text().splitlines() if l]
    p = m.profile()
    expected = s.checksum(s.shape(p,s.random_symbols(p,4103,2026)[3:]))
    bench = {r['test']:r for r in rows if r.get('test') in ('short4_c','short4_pie')}
    assert set(bench) == {'short4_c','short4_pie'}
    for row in bench.values(): assert row['check'] == expected, row
    assert bench['short4_pie']['cycles_min'] <= 33750, bench['short4_pie']
    for mc, crc in ((13,3178964763),(15,4256019779)):
        dumps = [r for r in rows if r.get('cmd')=='dvbdump' and r.get('modcod')==mc
                 and r.get('pulse_symbols')==6]
        assert dumps, (mc,'missing dump')
        for r in dumps:
            assert r['crc32']==crc and r['overlap_mismatches']==0, r
            assert r['status']=='ESP_OK' and not r['encoder_failed'], r
        paced = [r for r in rows if r.get('cmd')=='dvbpace' and r.get('modcod')==mc
                 and r.get('pulse_symbols')==6 and r.get('stall_ms')==0]
        assert paced, (mc,'missing pace')
        for r in paced:
            assert r['status']=='ESP_OK' and not r['encoder_failed'], r
            for k in ('late_fills','late_switches','fatal_late','underruns'):
                assert r[k]==0, (k,r)
            assert r['max_fill_cycles']<=40137 and r['min_fill_slack_cycles']>=7083, r
            assert r['encode_cycles_live_max']<=226378, r
            assert r['ring_heap_bytes']==0 and r['ring_slots']==3, r
            assert r['dac_rate']==80000000 and r['step_words']==15740, r
            assert r['profile']['shaped_words']+r['profile']['reused_words']==r['profile']['fill_words'], r
    print('round2 board: kernel, CRCs, overlap, pacing and 15% timing gates pass')


def main():
    assert m.quarter_tables().nbytes == 18432
    assert np.array_equal(m.unfold_quarters(m.quarter_tables()),
                          np.moveaxis(m.maxrate.parts(),0,-1))
    g = s.corrected_rrc(.35,4,span=6)
    assert np.array_equal(np.round(g*508/abs(g.reshape(6,4)).sum(0).max()*256), m.PULSE4)
    for level in (16,128,508):
        p = m.profile(level)
        for axis in range(2):
            values = p.parts[axis] + p.biases[axis][:,None,:]
            assert values.min() >= 0 and values.max(1).sum(0).max() < 1024
    with tempfile.TemporaryDirectory() as tmp:
        pathlib.Path(tmp,'esp_attr.h').write_text('#define IRAM_ATTR\n')
        exe = pathlib.Path(tmp,'round2')
        subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-DROUND2_HOST','-I'+tmp,
                        '-I'+str(ROOT/'firmware/main'), str(ROOT/'tests/round2.c'),
                        str(ROOT/'firmware/main/shaper.c'),str(ROOT/'firmware/main/shaper_short.c'),
                        '-o',str(exe)], check=True)
        records = [json.loads(x[1:]) for x in subprocess.check_output([str(exe)],text=True).splitlines()]
        check(records)
    if len(sys.argv)>1 and sys.argv[1]=='--board':
        check_board(pathlib.Path(sys.argv[2]))
    elif len(sys.argv)>1:
        rows = [json.loads(x[1:]) for x in pathlib.Path(sys.argv[1]).read_text().splitlines()
                if x.startswith('@{"test":"round2"')]
        check(rows)
        assert rows == records
    # Conservation of work and empty/fully occupied limiting cases.
    assert m.budget(0,0,3)['combined_margin'] == 1
    assert m.budget(47220,199746,3)['combined_margin'] == 0
    assert abs(m.budget(40137,226378.8,4)['combined_margin']-.15) < 1e-12
    print('round2: 162 cases, independent pulse/tables, bounds, C/Python and optional PIE agree')
if __name__ == '__main__': main()
