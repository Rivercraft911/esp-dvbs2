"""Renderer residency/cleanup and the linked SRAM budget. No board."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def symbols(path):
    return {name: int(addr,16) for addr,name in re.findall(
        r'^\s*0x([0-9a-f]+)\s+([A-Za-z_]\w*)\b',path.read_text(),re.M)}


def check(current, baseline=None):
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp)
        (out/'esp_attr.h').write_text('#define IRAM_ATTR\n')
        subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-I'+tmp,
                        str(ROOT/'tests/lofty_resident.c'),'-o',str(out/'resident')],check=True)
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
    store = m['shaper_store']
    assert store%16 == 0 and 0x3fc88000 <= store < store+65536 <= 0x3fcc0000
    print(f'linked: {size} B locked only for 26m667; renderer IRAM; common 64 KiB store before DAC')
    if baseline:
        b = symbols(baseline)
        delta = m['_heap_start']-b['_heap_start']
        iram = m['_iram_text_end']-b['_iram_text_end']
        assert delta <= 1024,delta
        print(f'versus main: {delta} B static SRAM growth ({iram} B IRAM, {delta-iram} B data/alignment)')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--map',type=Path)
    p.add_argument('--main-map',type=Path)
    a = p.parse_args()
    check(a.map,a.main_map)
