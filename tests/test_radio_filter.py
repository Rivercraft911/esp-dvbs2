"""Native fault injection for the isolated LPF-B experiment; no hardware access."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RADIO = ROOT / "firmware/components/radio_s3"


def test():
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp)
        (out / "esp_private").mkdir()
        (out / "soc").mkdir()
        (out / "esp_private/regi2c_ctrl.h").write_text("""#include <stdint.h>
uint8_t regi2c_ctrl_read_reg(unsigned,unsigned,unsigned);
void regi2c_ctrl_write_reg_mask(unsigned,unsigned,unsigned,unsigned,unsigned,unsigned);
""")
        (out / "soc/soc.h").write_text("""#include <stdint.h>
uint32_t fake_read(unsigned);
#define REG_READ(a) fake_read(a)
""")
        source = out / "filter.c"
        source.write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "radio_filter.h"
static uint8_t caps[16];
static unsigned wide, writes, ignore_pair, ignore_restore;
uint32_t fake_read(unsigned addr) {
    if (addr==0x60006100u) return wide ? 0x00120000 : 0;
    if (addr==0x6002600cu) return wide ? 4 : 0;
    if (addr==0x60033400u) return wide ? 3 : 0;
    assert(addr==0x6001d000u || addr==0x6001cc0cu); return 0;
}
uint8_t regi2c_ctrl_read_reg(unsigned b,unsigned h,unsigned r) {
    assert(b==0x67 && !h && r<16); return caps[r];
}
void regi2c_ctrl_write_reg_mask(unsigned b,unsigned h,unsigned r,unsigned msb,unsigned lsb,unsigned v) {
    assert(b==0x67 && !h && (r==14 || r==15) && msb==5 && !lsb);
    ++writes;
    if ((v==2 && (ignore_pair & (1u<<(r-14)))) || (v==10 && ignore_restore)) return;
    caps[r]=(caps[r]&0xc0u)|v;
}
void rom_bb_bss_cbw40(unsigned mode) { assert(mode==0 || mode==2); wide=mode==2; }
int main(void) {
    memset(caps,31,sizeof caps); caps[14]=0x8a; caps[15]=0xca;
    for (ignore_pair=0; ignore_pair<4; ++ignore_pair) {
        writes=0;
        radio_filter_stats_t s={.occupied_hz=60000000};
        assert(radio_filter_apply(&s)==(!RADIO_TX_LPFB2 || !ignore_pair));
        assert(s.caps_overridden==!!RADIO_TX_LPFB2);
        assert(writes==(RADIO_TX_LPFB2 ? 2u : 0u));
        assert(radio_filter_finish(&s)==(!RADIO_TX_LPFB2 || !ignore_pair));
        assert(s.restored && caps[14]==0x8a && caps[15]==0xca);
        assert(writes==(RADIO_TX_LPFB2 ? 4u : 0u));
    }
    ignore_pair=0; writes=0;
    radio_filter_stats_t narrow={.occupied_hz=13500000};
    assert(radio_filter_apply(&narrow) && !narrow.caps_overridden && !writes);
    assert(radio_filter_finish(&narrow));
    if (RADIO_TX_LPFB2) {
        caps[14]=11;
        radio_filter_stats_t mismatch={.occupied_hz=60000000};
        assert(!radio_filter_apply(&mismatch) && !mismatch.caps_overridden && !writes);
        caps[14]=0x8a;
        radio_filter_stats_t failed={.occupied_hz=60000000};
        assert(radio_filter_apply(&failed)); ignore_restore=1;
        assert(!radio_filter_finish(&failed) && failed.retained && !failed.restored);
    }
    puts("LPF-B: both builds, partial writes, upper bits, narrow path, baseline and restore failures pass");
}
''')
        for enabled in (0, 1):
            exe = out / f"filter-{enabled}"
            subprocess.run(["cc", "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            f"-DRADIO_TX_LPFB2={enabled}", "-I"+tmp, "-I"+str(RADIO / "include"),
                            str(source), str(RADIO / "radio_filter.c"), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    test()
