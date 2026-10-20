"""Exercise the actual optional profile against register/PBUS failures. No hardware."""
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RADIO = ROOT / "firmware/components/radio_s3"
MODEL = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "radio_calibration.h"
static const unsigned shifts[2]={9,18};
static uint32_t regs[3], saved[3];
static uint8_t mac[6]={0x44,0xb1,0x76,0xb0,0xe5,0x8c};
static unsigned reads,writes,pbus,mac_reads,filters,fail_iq,fail_pbus;
static bool filter_ok=true;
static radio_calibration_stats_t attempt;
static esp_err_t begin(uint32_t mhz,unsigned gain,uint32_t rate) {
    radio_calibration_attempt(&attempt,true);
    return radio_calibration_begin(mhz,gain,rate,&attempt);
}
static unsigned index_of(uint32_t reg) {
    if(reg==0x6000607cU) return 0;
    if(reg==0x6000611cU) return 1;
    assert(reg==0x60006120U); return 2;
}
uint32_t model_read(uint32_t reg) { ++reads; return regs[index_of(reg)]; }
void model_write(uint32_t reg,uint32_t word) {
    assert(reg==0x6000607cU); ++writes;
    regs[0]=word; if(fail_iq) { regs[0]^=1; fail_iq=0; }
}
esp_err_t esp_efuse_mac_get_default(uint8_t *out) {
    ++mac_reads; memcpy(out,mac,6); return ESP_OK;
}
bool radio_filter_apply(radio_filter_stats_t *stats) { (void)stats; ++filters; return filter_ok; }
void rom_pbus_force_test(uint8_t group,uint8_t pair,uint16_t value) {
    assert((group==2||group==3)&&pair==1&&value<=511); ++pbus;
    unsigned i=group-2; uint32_t mask=511U<<shifts[i];
    if(fail_pbus==pbus) value^=1;
    regs[i+1]=(regs[i+1]&~mask)|((uint32_t)value<<shifts[i]);
}
static void baseline(void) {
    regs[0]=0xa5c31abcU;
    regs[1]=0x84000000U|(250U<<9)|51U;
    regs[2]=0x90000000U|(260U<<18)|(231U<<9)|7U;
    memcpy(saved,regs,sizeof regs); reads=writes=pbus=mac_reads=filters=0;
    fail_iq=fail_pbus=0; filter_ok=true;
}
static void emit(const char *name,const radio_calibration_stats_t *stats) {
    printf("@{\"case\":\"%s\"",name); radio_calibration_print(stats); puts("}");
}
static void result(const char *name) {
    radio_calibration_end(&attempt); emit(name,&attempt);
}
int main(void) {
    radio_filter_stats_t filter={0};
    baseline();
    // Default OFF does not read MAC/registers or write calibration, even at an
    // unrelated operating point. The original filter setup still executes.
    assert(begin(2430,7,80000000)==ESP_OK);
    assert(radio_calibration_filter(&filter)&&radio_calibration_restore());
    assert(!reads&&!writes&&!pbus&&!mac_reads&&filters==1);
    result("default");
    radio_calibration_command("bench2420");
    const unsigned bad[][3]={{2000,0,40000000},{2025,0,40000000},{2110,0,40000000},
                            {2200,0,40000000},{2290,0,40000000},{2400,0,40000000},
                            {2419,0,40000000},{2420,1,40000000},{2420,0,80000000}};
    for(unsigned i=0;i<sizeof bad/sizeof *bad;++i) {
        baseline(); assert(begin(bad[i][0],bad[i][1],bad[i][2])!=ESP_OK);
        assert(!radio_calibration_filter(&filter)&&radio_calibration_restore());
        assert(!reads&&!writes&&!pbus&&!filters);
    }
    baseline(); mac[5]^=1;
    assert(begin(2420,0,40000000)!=ESP_OK);
    assert(!reads&&!writes&&!pbus); mac[5]^=1;
    result("guard");
    // Invalid command cannot silently turn the profile off.
    radio_calibration_command("bench2420 trailing");
    baseline(); assert(begin(2420,0,40000000)==ESP_OK);
    filter_ok=false; assert(!radio_calibration_filter(&filter));
    assert(radio_calibration_restore()&&!reads&&!writes&&!pbus);
    baseline(); regs[1]=(regs[1]&~(511U<<9))|(200U<<9);
    assert(begin(2420,0,40000000)==ESP_OK);
    assert(!radio_calibration_filter(&filter)&&!writes&&!pbus);
    // Apply exact absolute coordinates, then restore original fields. A change
    // to an unrelated field during a run must survive restoration.
    baseline(); assert(begin(2420,0,40000000)==ESP_OK);
    assert(radio_calibration_filter(&filter));
    assert(regs[0]==((saved[0]&~0x1fffU)|0x800U|3U|(52U<<5)));
    assert(regs[1]==((saved[1]&~(511U<<9))|(246U<<9)));
    assert(regs[2]==((saved[2]&~(511U<<18))|(264U<<18)));
    regs[0]^=0x80000000U; regs[1]^=1; regs[2]^=1;
    assert(radio_calibration_restore());
    assert(regs[0]==(saved[0]^0x80000000U)&&regs[1]==(saved[1]^1)&&regs[2]==(saved[2]^1));
    result("matched");
    // A new API attempt that fails before worker admission cannot inherit the
    // completed run. Initializing another result must not mutate that run.
    radio_calibration_stats_t early, nonrf;
    radio_calibration_attempt(&early,true);
    assert(early.requested&&!early.guarded&&!early.applied&&!early.restored);
    emit("early_admission",&early);
    assert(attempt.applied&&attempt.restored);
    radio_calibration_attempt(&nonrf,false);
    assert(!nonrf.requested); emit("nonrf",&nonrf);
    const unsigned before=writes+pbus;
    assert(radio_calibration_restore()&&writes+pbus==before);
    // A fresh session restores its own native values, not the last run's values.
    saved[1]=regs[1]; saved[2]=regs[2]; saved[0]=regs[0];
    assert(begin(2420,0,40000000)==ESP_OK);
    assert(radio_calibration_filter(&filter)&&radio_calibration_restore());
    assert(!memcmp(regs,saved,sizeof regs)); result("repeat");
    // Fail each write independently. Cleanup restores every field already
    // attempted, including the write whose readback failed.
    for(unsigned fault=0;fault<3;++fault) {
        baseline(); fail_iq=fault==0; fail_pbus=fault;
        assert(begin(2420,0,40000000)==ESP_OK);
        assert(!radio_calibration_filter(&filter));
        assert(radio_calibration_restore());
        assert(!memcmp(regs,saved,sizeof regs)); result("partial");
    }
    baseline(); assert(begin(2420,0,40000000)==ESP_OK);
    assert(radio_calibration_filter(&filter)); fail_pbus=pbus+1;
    assert(!radio_calibration_restore()); result("restore_failure");
    radio_calibration_command("off");
    baseline(); assert(begin(2421,3,80000000)==ESP_OK);
    assert(radio_calibration_filter(&filter)&&!reads&&!writes&&!pbus&&!mac_reads);
    result("off");
}
'''


def main():
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp)
        (folder / "soc").mkdir()
        (folder / "esp_err.h").write_text(
            "#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_ERR_NOT_SUPPORTED 0x106\n")
        (folder / "esp_mac.h").write_text(
            '#include <stdint.h>\n#include "esp_err.h"\nesp_err_t esp_efuse_mac_get_default(uint8_t *mac);\n')
        (folder / "soc/soc.h").write_text(
            "#include <stdint.h>\nuint32_t model_read(uint32_t);\nvoid model_write(uint32_t,uint32_t);\n"
            "#define REG_READ(r) model_read(r)\n#define REG_WRITE(r,v) model_write(r,v)\n")
        source = folder / "profile.c"
        source.write_text(MODEL)
        exe = folder / "profile"
        subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-I" + tmp, "-I" + str(RADIO / "include"), str(source),
                        str(RADIO / "radio_calibration.c"), "-o", str(exe)], check=True)
        output = subprocess.check_output([str(exe)], text=True)
    rows = [json.loads(line[1:]) for line in output.splitlines() if line.startswith("@")]
    cases = {row["case"]: row for row in rows if "case" in row}
    assert "rfprofile" not in cases["default"] and "rfprofile" not in cases["off"]
    assert not cases["guard"]["rfprofile"]["guarded"]
    assert "rfprofile" not in cases["nonrf"]
    early = cases["early_admission"]["rfprofile"]
    assert not early["guarded"] and not early["applied"] and not early["restored"]
    for name in ("matched", "repeat"):
        assert cases[name]["rfprofile"]["applied"] and cases[name]["rfprofile"]["restored"]
    partial = [row["rfprofile"] for row in rows if row.get("case") == "partial"]
    assert len(partial) == 3 and all(not r["applied"] and r["restored"] for r in partial)
    assert not cases["restore_failure"]["rfprofile"]["restored"]
    print("RF profile: default off, operating-point guards, absolute PBUS values, bit preservation, partial failure and restore: ok")


if __name__ == "__main__":
    main()
