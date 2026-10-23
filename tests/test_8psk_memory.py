"""Console fixture parser, post-Wi-Fi queue sizing and heap failures. No board."""
import argparse
import json
import pathlib
from pathlib import Path
import re
import subprocess
import tempfile
from test_stream import ROOT, compile_stream


def check(out):
    out.mkdir(parents=True,exist_ok=True)
    source = (ROOT/'firmware/tx/main/main.c').read_text()
    functions = '\n'.join(re.search(r'static (?:bool|void) '+name+r'\([^\n]*[\s\S]*?\n}',source).group()
                          for name in ('number','tx_mode','short_scope','take_option','cmd_dvb','cmd_dvbpace'))
    parser = out/'parser.c'
    parser.write_text('''#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dvbtx.h"
#define RADIO_S3_DAC_RATE 40000000u
#define RADIO_S3_DAC_RATE_80M 80000000u
#define RADIO_S3_FREQ_MIN_MHZ 2000u
#define RADIO_S3_FREQ_MAX_MHZ 2450u
#define RADIO_S3_GAIN_DEFAULT 0
#define RADIO_S3_GAIN_MAX 17
#define RADIO_S3_MAX_SECONDS 540u
#define RADIO_S3_PHY_SOLO_MAX_SECONDS 60u
static bool station_require(const char *cmd) { (void)cmd; return true; }
static unsigned keyed_calls;
static bool expected_solo;
static void dvbtx_run_check(uint32_t freq, unsigned mc, uint32_t sec, unsigned gain,
    unsigned peak, unsigned sps, uint32_t rate, bool fixture, bool solo)
{
    assert(freq == 2412 && sec == 30 && !gain && peak == 128 && rate == 80000000);
    assert((mc == 11 && sps == DVB_SPS_12_5) || ((mc == 13 || mc == 15 || mc == 17) && sps == 3));
    assert(fixture == (keyed_calls < 4 || keyed_calls >= 5));
    assert(solo == expected_solo); ++keyed_calls;
}
#define dvbtx_run dvbtx_run_check
static unsigned calls;
static bool expected_pace_fixture;
static void dvbtx_pace_check(unsigned mc, uint32_t sec, unsigned peak, unsigned stall,
    unsigned sps, uint32_t rate, bool fixture, bool solo)
{
    assert(mc == 17 && (sec == 10 || sec == 600) && peak == 128 && !stall && sps == 3 && rate == 80000000);
    assert(solo == expected_solo && fixture == expected_pace_fixture); ++calls;
}
#define dvbtx_pace dvbtx_pace_check
void dvbtx_set_short(bool enabled) { assert(!enabled); }
'''+functions+'''
int main(void) {
    const char *cases[] = {"17 26m667 10", "17 26m667 10 128 0",
        "17 26m667 10 128 0 trace", "17 26m667 10 128 0 pages=54",
        "17 26m667 10 128 0 nowifi", "17 26m667 10 128 0 observe",
        "17 26m667 10 128 0 pagewake"};
    for (unsigned i = 0; i < sizeof cases/sizeof *cases; ++i) {
        char text[128]; strcpy(text,cases[i]); cmd_dvbpace(text);
        assert(calls == (i < 2 ? i+1 : 2));
    }
    puts("console: normal pace arguments accepted; retired diagnostics rejected: ok");
    const char *keyed[] = {"2412 13 26m667 30 0 128 --fixture",
        "--fixture 2412 15 26m667 30", "2412 17 26m667 --fixture 30",
        "2412 11 33m333 30 --fixture", "2412 13 26m667 30",
        "2412 13 26m667 30 --fixture --fixture", "2412 13 26m667 30 --phy-solojunk",
        "2412 11 20m 30 --fixture", "2412 13 20m 30 --fixture",
        "2412 13 26m667 30 --fixturejunk", "13 26m667 30 --fixture"};
    for (unsigned i = 0; i < sizeof keyed/sizeof *keyed; ++i) {
        char text[128]; strcpy(text,keyed[i]); cmd_dvb(text);
        assert(keyed_calls == (i < 5 ? i+1 : 5));
    }
    expected_solo = true;
    char keyed_option[] = "--fixture 2412 13 26m667 30 --phy-solo";
    char pace_option[] = "17 26m667 10 --phy-solo";
    cmd_dvb(keyed_option); cmd_dvbpace(pace_option);
    assert(keyed_calls == 6 && calls == 3);
    char duplicate[]="2412 13 26m667 30 --phy-solo --phy-solo";
    char duplicate_solo[]="17 26m667 10 --phy-solo --phy-solo";
    char retired[]="2412 13 26m667 30 --fused";
    char too_long[]="17 26m667 601 --phy-solo";
    cmd_dvb(duplicate); cmd_dvbpace(duplicate_solo); cmd_dvb(retired); cmd_dvbpace(too_long);
    assert(keyed_calls == 6 && calls == 3);
    char long_solo[]="17 26m667 600 --phy-solo"; // paces are not capped at the keyed PHY-solo limit
    cmd_dvbpace(long_solo); assert(calls == 4);
    expected_pace_fixture = true; expected_solo = false;
    char pace_fixture[]="17 26m667 10 --fixture";
    char short_fixture[]="16 5m 10 --fixture --short";
    cmd_dvbpace(pace_fixture); cmd_dvbpace(short_fixture); assert(calls == 5);
    puts("keyed fixture: flag order, QPSK/8PSK rates, defaults, duplicates and unsupported options: ok");
}
''')
    subprocess.run(['cc','-O2','-std=c11','-Wall','-Wextra','-Werror',
                    '-I'+str(ROOT/'firmware/tx/main'),str(parser),'-o',str(out/'parser')],check=True)
    text = subprocess.check_output([str(out/'parser')],text=True)
    (out/'parser.log').write_text(text)
    print(text.splitlines()[-1])
    exe = compile_stream(out,source='psk8_memory.c')
    run = subprocess.run([str(exe)],capture_output=True,text=True,timeout=60)
    (out/'lifecycle.log').write_text(run.stdout+run.stderr)
    assert run.returncode == 0,(run.stdout,run.stderr)
    rows = [json.loads(l[1:]) for l in run.stdout.splitlines() if l.startswith('@{')]
    pace = [r for r in rows if r.get('cmd') == 'dvbpace' and 'status' in r]
    assert len(pace) == 2 and all(r['status'] == 'ESP_OK' and r['paced_wifi_up'] for r in pace)
    assert not pace[0]['profile']['fill_phase_cycles'] and not pace[0]['profile']['fill']['whole']['calls']
    assert pace[1]['profile']['fill_phase_cycles'] and pace[1]['profile']['fill']['whole']['calls'] > 0
    budget = [r for r in rows if r.get('test') == 'queue_budget']
    assert len(budget) == 18
    for mc in (13,15,17):
        for setup in ('dvb','dvbpace'):
            pair = [r for r in budget[:12] if r['modcod'] == mc and r['setup'] == setup]
            assert [r['queue_sizing']['automatic'] for r in pair] == [False,True]
            assert [r['queue_pages'] for r in pair] == [{13:54,15:50,17:46}[mc],54]
            assert pair[0]['crc'] == pair[1]['crc']
            auto = pair[1]
            assert auto['queue_sizing']['free_before'] == 32000
            assert auto['queue_sizing']['free_after'] == 32000
            assert auto['renderer_warm_words'] == 39983 and pair[0]['renderer_warm_words'] == 0
            assert auto['dummy_dcache_bytes'] == 13376 and not pair[0]['dummy_dcache_bytes']
            assert not auto['ring_heap_bytes'] and auto['workspace_heap_bytes'] == 8144
            layout = auto['queue_layout']
            assert layout['contiguous'] and layout['resident_pages'] == 54 and layout['page_bytes'] == 528
            region, = layout['regions']
            assert region['first_page'] == 0 and region['pages'] == 54 and not region['dac_overlap']
            assert int(region['end'],16)-int(region['begin'],16) == 54*528
            if setup == 'dvbpace':
                assert auto['input_memory'] == 'psram' and auto['reference_psram_bytes'] > 5000
                assert auto['reference_psram_bytes'] == {13:5370,15:6720,17:7264}[mc]
                assert not auto['reference_store_bytes'] and int(auto['layout']['reference'],16)
            else: assert auto['input_memory'] == 'none' and not auto['reference_psram_bytes']
            assert int(auto['layout']['dummy'],16)
            assert int(auto['layout']['encoder_cache_tail'],16)
            assert auto['encoder_cache_psram_bytes'] == {13:2912,15:2720,17:2320}[mc]
            assert auto['encoder_cache_heap_bytes']+auto['encoder_cache_psram_bytes'] == auto['encoder_cache_bytes']
    assert [r['queue_pages'] for r in budget[-5:]] == [54,54,0,0,54]
    assert [r['queue_sizing']['ready'] for r in budget[-3:]] == [False,False,True]
    fragmented = [r for r in rows if r.get('test') == 'queue_fragmentation']
    assert [(r['modcod'],r['setup']) for r in fragmented] == [(mc,s) for mc in (13,15,17) for s in ('dvb','dvbpace')]
    assert all(r['queue_pages'] == 54 for r in fragmented)
    for r in fragmented:
        assert r['crc'] == next(b['crc'] for b in budget if (b['modcod'],b['setup']) == (r['modcod'],r['setup']))
    print('fragmented heap: old order fails 15/17, cache-first order streams all three with identical words in pace and keyed setups')
    print('contiguous queues: 13/15/17 pace and keyed setup 54 pages; no post-Wi-Fi allocations; exact reserve and failure cleanup')
    print(run.stdout.splitlines()[-1])


def symbols(path):
    return {name: int(addr,16) for addr,name in re.findall(
        r'^\s*0x([0-9a-f]+)\s+([A-Za-z_]\w*)\b',path.read_text(),re.M)}


def check_residency(current):
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp)
        (out/'esp_attr.h').write_text('#define IRAM_ATTR\n')
        subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-I'+tmp,
                        '-I'+str(ROOT/'firmware/components/shaper/include'),
                        str(ROOT/'tests/psk8_resident.c'),'-o',str(out/'resident')],check=True)
        subprocess.run([str(out/'resident')],check=True)
    print('cache residency: dormant, timeout, ROM error, repeated setup and cleanup: ok')
    if not current: return
    m = symbols(current)
    project = json.loads((current.parent/'project_description.json').read_text())
    nm = project['c_compiler'].removesuffix('gcc')+'nm'
    for line in subprocess.check_output([nm,str(current.with_suffix('.elf'))],text=True).splitlines():
        fields = line.split()
        if len(fields) == 3: m[fields[2]] = int(fields[0],16)
    first,end = m['_lofty_text_start'],m['_lofty_text_end']
    size = (end-first+31)&~31
    assert first%32 == 0 and 0 < size <= 4096
    assert 0x42000000 <= first < end < 0x42800000
    for name in ('lofty_shape','lofty_short','lofty_splice','lofty_samples'):
        assert first <= m[name] < end,(name,m[name])
    assert 0x42000000 <= m['lofty_prepare_pie'] < 0x42800000
    for name in ('lofty_render_pie','lofty_render_pages_pie','machine_axis_render_pie',
                 'machine_axis_samples','xTaskGenericNotify','shaper_copy_pie'):
        assert 0x40370000 <= m[name] < 0x403e0000,(name,m[name])
    assert m['lofty_render_pages_pie'] % 32 == 4, "renderer fetch alignment must match 7391f86"
    assert m['lofty_render_pie'] % 32 == 20
    assert first % 4096 == 0x720, "lock cache indices must match 7391f86"
    state = m['_stream_bss_start']
    assert state % 16 == 0 and 0x3fc88000 <= state < m['_stream_bss_end'] <= 0x3fcc0000
    assert 0x40370000 <= m['_vcm_text_start'] < m['_vcm_text_end'] < 0x403e0000
    assert m['shaper_axes8_pie'] % 32 == 8, "axis PIE fetch alignment must match modswap 154692d"
    assert m['_vcm_text_end'] <= m['_iram_text_end'] <= m['_iram_end']
    assert m['_data_start'] >= m['_iram_end'] - (0x40378000-0x3fc88000)
    for name in ('fill_vcm', 'ring_read_vcm', 'ring_window_vcm', 'shaper_axes8_pie', 'shaper_axes_samples_profile'):
        entries = [address for symbol, address in m.items() if symbol == name or symbol.startswith(name+'$') or symbol.startswith(name+'.')]
        assert entries and all(m['_vcm_text_start'] <= address < m['_vcm_text_end'] for address in entries), name
    assert not any(name == 'lofty_page_at' or name.startswith('lofty_page_at.') for name in m), "page accessor must stay inline"
    store = m['shaper_store']
    assert store%16 == 0 and 0x3fc88000 <= store < store+65536 <= 0x3fcc0000
    print(f'linked: renderer fetch alignment, {size} B lock with 7391f86 cache indices; separate VCM IRAM; stream state and 64 KiB store before DAC')

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('output',nargs='?',type=pathlib.Path)
    p.add_argument('--map',type=pathlib.Path)
    a = p.parse_args()
    check_residency(a.map)
    with tempfile.TemporaryDirectory() as tmp:
        check((a.output or pathlib.Path(tmp)).resolve())
