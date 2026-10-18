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
                          for name in ('number','tx_mode','cmd_dvbpace'))
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
    print(run.stdout.splitlines()[-1])


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('output',type=pathlib.Path)
    check(p.parse_args().output.resolve())
