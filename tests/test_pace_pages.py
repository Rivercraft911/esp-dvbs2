"""Actual console parser and pace lifecycle with runtime pages. No board."""
import argparse
import json
import pathlib
import re
import subprocess
from test_stream import ROOT, compile_stream


def check(out):
    out.mkdir(parents=True,exist_ok=True)
    source = (ROOT/'firmware/main/main.c').read_text()
    functions = '\n'.join(re.search(r'static (?:bool|void) '+name+r'\([^\n]*[\s\S]*?\n}',source).group()
                          for name in ('number','tx_mode','take_option','cmd_dvb','cmd_dvbpace'))
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
#define RADIO_S3_GAIN_DEFAULT 0
#define RADIO_S3_GAIN_MAX 17
static bool station_require(const char *cmd) { (void)cmd; return true; }
static unsigned keyed_calls;
static void dvbtx_run_check(uint32_t freq, unsigned mc, uint32_t sec, unsigned gain,
    unsigned peak, unsigned sps, uint32_t rate, bool fixture)
{
    assert(freq == 2412 && sec == 30 && !gain && peak == 128 && rate == 80000000);
    assert((mc == 11 && sps == DVB_SPS_12_5) || ((mc == 13 || mc == 15 || mc == 17) && sps == 3));
    assert(fixture == (keyed_calls < 4)); ++keyed_calls;
}
#define dvbtx_run dvbtx_run_check
static unsigned calls;
static void dvbtx_pace_check(unsigned mc, uint32_t sec, unsigned peak, unsigned stall,
    unsigned sps, uint32_t rate, unsigned wake, bool observe, bool trace, unsigned pages, bool nowifi)
{
    assert(mc == 17 && sec == 10 && peak == 128 && !stall && sps == 3 && rate == 80000000);
    assert(!observe && !trace && !wake && (pages == 0 || pages == 58));
    assert(nowifi == (pages == 58)); ++calls;
}
#define dvbtx_pace dvbtx_pace_check
'''+functions+'''
int main(void) {
    const char *cases[] = {"17 26m667 10", "17 26m667 10 128 0 pages=58 nowifi",
        "17 26m667 10 128 0 nowifi pages=58", "17 26m667 10 128 0 pages=43",
        "17 26m667 10 128 0 pages=65", "17 26m667 10 128 0 pages=58junk",
        "17 26m667 10 128 0 pages=58 pages=58", "17 26m667 10 128 0 nowifi nowifi",
        "17 26m667 10 128 0 observe nowifi", "17 26m667 10 128 0 pages=",
        "17 26m667 10 128 0 pages=0", "17 26m667 10 128 0 pages=-1"};
    for (unsigned i = 0; i < sizeof cases/sizeof *cases; ++i) {
        char text[128]; strcpy(text,cases[i]); cmd_dvbpace(text);
        assert(calls == (i < 3 ? i+1 : 3));
    }
    puts("console: defaults, flag order, strict page range, duplicates and incompatible flags: ok");
    const char *keyed[] = {"2412 13 26m667 30 0 128 --fixture",
        "--fixture 2412 15 26m667 30", "2412 17 26m667 --fixture 30",
        "2412 11 33m333 30 --fixture", "2412 13 26m667 30",
        "2412 13 26m667 30 --fixture --fixture", "2412 13 26m667 30 --phy-solo",
        "2412 11 20m 30 --fixture", "2412 13 20m 30 --fixture",
        "2412 13 26m667 30 --fixturejunk", "13 26m667 30 --fixture"};
    for (unsigned i = 0; i < sizeof keyed/sizeof *keyed; ++i) {
        char text[128]; strcpy(text,keyed[i]); cmd_dvb(text);
        assert(keyed_calls == (i < 5 ? i+1 : 5));
    }
    puts("keyed fixture: flag order, QPSK/8PSK rates, defaults, duplicates and unsupported options: ok");
}
''')
    subprocess.run(['cc','-O2','-std=c11','-Wall','-Wextra','-Werror',
                    '-I'+str(ROOT/'firmware/main'),str(parser),'-o',str(out/'parser')],check=True)
    text = subprocess.check_output([str(out/'parser')],text=True)
    (out/'parser.log').write_text(text)
    print(text.splitlines()[-1])
    exe = compile_stream(out,source='lofty_pace.c')
    run = subprocess.run([str(exe)],capture_output=True,text=True,timeout=60)
    (out/'lifecycle.log').write_text(run.stdout+run.stderr)
    assert run.returncode == 0,(run.stdout,run.stderr)
    rows = [json.loads(l[1:]) for l in run.stdout.splitlines() if l.startswith('@{')]
    pages = [r for r in rows if r.get('test') == 'pace_pages']
    assert [r['pages'] for r in pages] == [46,46,50,54,58,44,64]
    assert len(set(r['crc'] for r in pages)) == 1
    pace = [r for r in rows if r.get('cmd') == 'dvbpace' and 'status' in r]
    assert len(pace) == 9 and all(r['status'] == 'ESP_OK' for r in pace)
    assert all(not r['paced_wifi_up'] and 'wifi_min_free' not in r for r in pace[:7])
    assert all(r['paced_wifi_up'] for r in pace[7:])
    budget = [r for r in rows if r.get('test') == 'queue_budget']
    assert len(budget) == 18
    for mc in (13,15,17):
        for setup in ('dvb','dvbpace'):
            pair = [r for r in budget[:12] if r['modcod'] == mc and r['setup'] == setup]
            assert [r['queue_sizing']['automatic'] for r in pair] == [False,True]
            assert all(r['queue_pages'] == 54 for r in pair)
            assert pair[0]['crc'] == pair[1]['crc']
            auto = pair[1]
            assert auto['queue_sizing']['free_before'] == 32000
            assert auto['queue_sizing']['free_after'] == 32000
            assert auto['renderer_warm_words'] == 39983 and pair[0]['renderer_warm_words'] == 0
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


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('output',type=pathlib.Path)
    check(p.parse_args().output.resolve())
