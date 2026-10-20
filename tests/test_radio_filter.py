"""Check filter selection, PBUS write order and fresh readbacks without hardware."""
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RADIO = ROOT / "firmware/components/radio_s3"
MODEL = r'''
#include "radio_filter.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint32_t analog,digital,channel,cbw,mac;
static uint16_t pbus;
static unsigned cap,fail_write,events;
static char calls[32];
static void event(char c) { assert(events+1<sizeof calls);calls[events++]=c;calls[events]=0; }
uint32_t stub_read(uint32_t a) {
    switch(a) {
    case 0x60006100:return analog;case 0x6002600c:return digital;case 0x60033400:return channel;
    case 0x6001d000:return cbw;case 0x6001cc0c:return mac;default:assert(0);return 0;
    }
}
uint8_t regi2c_ctrl_read_reg(uint8_t b,uint8_t host,uint8_t r) {
    assert(b==0x67&&host==0);return (uint8_t)(cap+r);
}
uint16_t rom_pbus_rd(uint8_t g,uint8_t i) { assert(g==4&&i==2);event('R');return pbus; }
void rom_bb_bss_cbw40(unsigned mode) {
    assert(mode==0||mode==2);event('C');
    analog=(analog&~0x003f0000U)|(mode?0x00120000U:0);
    digital=(digital&~12U)|(mode?4U:0);channel=(channel&~3U)|(mode?3U:0);
    if(mode) {cbw&=~64U;mac&=~1U;}
}
void rom_pbus_force_test(uint8_t g,uint8_t i,uint16_t value) {
    assert(g==4&&i==2&&(value==0||value==32));event('F');if(!fail_write)pbus=value;
}
static radio_filter_stats_t apply(unsigned width,unsigned before) {
    events=0;calls[0]=0;pbus=before;
    radio_filter_stats_t s={.occupied_hz=width,.requested=RADIO_FILTER_AUTO};
    bool ok=radio_filter_apply(&s);
    assert(!strcmp(calls,"RCFR")&&s.applied&&s.before.pbus_4_2==before);
    assert(ok==s.matches);return s;
}
int main(void) {
    unsigned controls=0;
    assert(radio_filter_get()==RADIO_FILTER_AUTO);
    radio_filter_stats_t s=apply(36000000,0);
    assert(s.matches&&s.selected==RADIO_FILTER_WIDE&&s.active.pbus_4_2==32&&radio_filter_finish(&s));++controls;
    s=apply(40000000,511);assert(s.matches&&s.before.pbus_4_2==511&&s.active.pbus_4_2==32);++controls;
    s=apply(20000000,32);assert(s.matches&&s.selected==RADIO_FILTER_NARROW&&s.active.pbus_4_2==0&&radio_filter_finish(&s));++controls;
    s=apply(20000001,0);assert(s.matches&&s.selected==RADIO_FILTER_WIDE);++controls;
    fail_write=1;s=apply(36000000,0);assert(!s.matches&&!radio_filter_finish(&s));++controls;
    s=apply(13500000,32);assert(!s.matches&&!radio_filter_finish(&s));++controls;fail_write=0;
    s=apply(36000000,0);pbus=0;assert(!radio_filter_finish(&s));++controls;
    s=apply(36000000,0);++cap;assert(!radio_filter_finish(&s));++controls;
    s=apply(13500000,32);digital^=4;assert(!radio_filter_finish(&s));++controls;
    s=apply(36000000,0);assert(radio_filter_finish(&s));
    printf("{\"case\":\"printed\"");radio_filter_print(&s);puts("}");
    printf("{\"controls\":%u,\"errors\":0}\n",controls);
}
'''


def main():
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp)
        (folder / "esp_private").mkdir()
        (folder / "soc").mkdir()
        (folder / "esp_private/regi2c_ctrl.h").write_text(
            "#include <stdint.h>\nuint8_t regi2c_ctrl_read_reg(uint8_t,uint8_t,uint8_t);\n")
        (folder / "soc/soc.h").write_text(
            "#include <stdint.h>\nuint32_t stub_read(uint32_t);\n"
            "#define REG_READ(addr) stub_read(addr)\n")
        source, exe = folder / "filter.c", folder / "filter"
        source.write_text(MODEL)
        subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-I" + tmp, "-I" + str(RADIO / "include"), str(source),
                        str(RADIO / "radio_filter.c"), "-o", str(exe)], check=True)
        output = subprocess.check_output([str(exe)], text=True)
    rows = [json.loads(line) for line in output.splitlines()]
    assert rows[-1] == {"controls": 9, "errors": 0}
    printed = rows[0]["filter"]
    assert printed["matches"] and printed["retained"]
    assert [printed[phase]["pbus_4_2"] for phase in ("before", "active", "end")] == [0, 32, 32]
    print("RF filter: PBUS ordering, wide/narrow reset, write faults and persistence: ok")


if __name__ == "__main__":
    main()
