"""Run production command/API bounds, station mux and schedule with synthetic clocks. No devices."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RADIO = ROOT / "firmware/components/radio_s3"
STATION = ROOT / "firmware/components/station_id"


def function(source, name):
    match = re.search(r"^[^\n]*\b(?:esp_err_t|geometry_t|bool|void) " + re.escape(name) + r"\(",source,re.M)
    start = match.start()
    at = source.index(name + "(",start)
    brace = source.index("{", at)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


MODEL = r'''
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "radio_s3.h"
#include "station_id.h"
#define IRAM_ATTR
#define DVB_SPS_12_5 12
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_TIMEOUT 0x107
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define configMAX_PRIORITIES 25
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define portSET_INTERRUPT_MASK_FROM_ISR() 0
#define portCLEAR_INTERRUPT_MASK_FROM_ISR(i) ((void)(i))
#define DAC_CTRL 0
#define BANK_USAGE 1
#define DAC_RATE_80M (1U << 15)
#define REG_WRITE(r,v) assert((r) == BANK_USAGE ? ((v) == 2 || (v) == 4) : ((r) == DAC_CTRL && (v) == 0))
typedef void (*TaskFunction_t)(void *);
typedef int SemaphoreHandle_t;
typedef int BaseType_t;
static int state_lock, done_storage;
static bool running;
static unsigned tasks, commands;
static const char *call = "N0CALL";
static const char *station_store_get(void) { return call; }
static bool station_require(const char *cmd) { (void)cmd; return call[0] != 0; }
static void *heap_caps_calloc(size_t n, size_t size, unsigned caps) {
    (void)caps; return calloc(n,size);
}
static void heap_caps_free(void *p) { free(p); }
static int xSemaphoreCreateBinaryStatic(int *p) { (void)p; return 1; }
static void xSemaphoreTake(int s, unsigned t) { assert(s == 1 && t == portMAX_DELAY); }
static int xTaskCreatePinnedToCore(TaskFunction_t task, const char *name, unsigned stack,
                                 void *arg, unsigned priority, void *handle, int core) {
    (void)name; (void)stack; (void)priority; (void)handle; assert(core == 1);
    ++tasks; task(arg); return pdPASS;
}
radio_filter_mode_t radio_filter_get(void) { return RADIO_FILTER_AUTO; }
static uint64_t clock_cycles;
static uint32_t cycles(void) { return (uint32_t)clock_cycles++; }
static void wait_until(uint32_t deadline) {
    int32_t left = (int32_t)(deadline-(uint32_t)clock_cycles);
    if (left > 0) clock_cycles += (uint32_t)left;
}
static int esp_task_wdt_reset(void) { return 0; }
'''

WORKERS = r'''
static station_id_t observed;
static uint32_t parsed_seconds;
static bool parsed_solo;
static int payload(uint32_t *dst, uint64_t first, size_t count, void *ctx) {
    (void)ctx;
    for (size_t i=0; i<count; ++i) dst[i] = (uint32_t)(first+i)^0x12345678U;
    return ESP_OK;
}
static void worker(void *arg) {
    run_t *run = arg;
    station_id_t *id = run->ctx;
    observed = *id;
    assert(run->samples == id->total_samples);
    if (run->limit_duration) {
        assert(run->samples <= (uint64_t)run->seconds*run->geo.rate);
        assert(run->samples <= STATION_PERIOD*id->scale);
        assert(run->stats->id_count == 2);
        assert(id->total_samples == id->payload_samples+2ULL*id->length);
        // Read payload through the actual mux, across 32-bit sample rollover
        // and the old minute boundary. No interior point may become CW.
        uint32_t dst[7];
        for (uint64_t at=id->length; at+7<id->total_samples-id->length;
             at += run->geo.rate) {
            assert(!station_id_fill(dst,at,7,id));
            for (unsigned i=0; i<7; ++i)
                assert(dst[i] == ((uint32_t)(at-id->length+i)^0x12345678U));
        }
        // Both CW edges must remain exactly the real CW generator's samples.
        uint32_t expected[7];
        const uint32_t tone = id->marks[0].start+STATION_EDGE*id->scale;
        assert(!station_id_fill(dst,tone,7,id));
        assert(!station_cw_fill(expected,tone,7,id));
        assert(expected[0]);
        assert(!memcmp(dst,expected,sizeof dst));
        assert(!station_id_fill(dst,id->total_samples-id->length+tone,7,id));
        assert(!memcmp(dst,expected,sizeof dst));
        assert(!station_id_fill(dst,id->total_samples,7,id));
        for (unsigned i=0; i<7; ++i) assert(!dst[i]);
    }
    run->error = ESP_OK;
}
static void paced_worker(void *arg) { worker(arg); }
static esp_err_t fill_bank(run_t *run, unsigned bank, uint64_t start) {
    assert(bank <= 1 && start < run->samples);
    ++run->stats->fills; return ESP_OK;
}
static void dvbtx_run_options(uint32_t f, unsigned m, uint32_t seconds, unsigned g,
                            unsigned p, unsigned sps, uint32_t rate, bool fixture,
                            bool solo, bool fused) {
    (void)f; (void)m; (void)g; (void)p; (void)sps; (void)rate; (void)fixture; (void)fused;
    ++commands; parsed_seconds = seconds; parsed_solo = solo;
}
'''

COUNTERS = r'''
#include "ingress.h"
#include "ring.h"
static void ready(void *ctx, int level) { (void)ctx; (void)level; }
static void rearm(void *ctx, ingress_desc *slot, bool restart) {
    (void)ctx; (void)slot; assert(!restart);
}
static void counters(void) {
    uint32_t buffers[INGRESS_SLOTS*INGRESS_SLOT_BYTES/4], table[1024];
    uint8_t out[PV_PAYLOAD_MAX];
    ingress_hw hw = {.ready=ready,.rearm=rearm};
    ingress_ring input;
    ingress_init(&input,(uint8_t *)buffers,&hw);
    pv_crc32_tables(table);
    // More than a 530 s, 9 Mb/s stream: 658 MB, seven 16-bit wraps.
    for (unsigned sequence=0; sequence<500000; ++sequence) {
        ingress_desc *d = &input.desc[input.head%INGRESS_SLOTS];
        assert(d->dw0&INGRESS_OWNER_DMA);
        memset(d->buf+PV_HEADER_BYTES,sequence&255,PV_PAYLOAD_MAX);
        for (unsigned k=0; k<PV_PAYLOAD_MAX; k+=PV_TS_PACKET)
            d->buf[PV_HEADER_BYTES+k] = 0x47;
        pv_seal(d->buf,PV_TS,(uint16_t)sequence,PV_PAYLOAD_MAX);
        d->dw0 = INGRESS_SLOT_BYTES | (PV_MSG_BYTES<<12) | INGRESS_EOF;
        assert(ingress_read(&input,out,97) == 97);
        assert(ingress_read(&input,out+97,sizeof out-97) == sizeof out-97);
        assert(!memcmp(out,d->buf+PV_HEADER_BYTES,sizeof out));
    }
    assert(input.st.msgs == 500000 && input.st.bytes == 658000000);
    assert(input.st.ts_packets == 3500000 && input.seq_next == (uint16_t)500000);
    assert(input.head == 500000 && !input.st.lost && !input.st.seq_back && !input.st.overruns);
    pv_crc32_tables(NULL);
    // Production packed ring reads at every 32-bit DAC-sample crossing,
    // plus the final 530 s timeline. Absolute sample positions stay 64-bit.
    enum {symbols=22194, history=6}; // normal 8PSK PLFRAME with pilots
    uint8_t data[2][symbols/2+16], filler[symbols/2+16] = {0}, window[2200];
    uint8_t *slots[] = {data[0],data[1]};
    for (unsigned crossing=0; crossing<=5; ++crossing) {
        uint64_t first = (crossing == 5 ? 530ULL*40000000 : (uint64_t)crossing<<32)+64;
        uint32_t frame = first/(8*symbols);
        ring_t ring;
        ring_init_slots(&ring,slots,2,filler,symbols);
        ring_packed_codes(&ring);
        ring.next_frame = ring.freed = ring.published = frame;
        ring.release_position = ring.next_position = frame;
        ring.chosen_end = (uint64_t)frame*symbols;
        for (unsigned f=frame; f<frame+2; ++f) {
            uint8_t *dst = ring_slot(&ring,f); assert(dst);
            for (unsigned k=0; k<symbols; k+=2)
                dst[k/2] = ((uint64_t)f*symbols+k)%8 | ((((uint64_t)f*symbols+k+1)%8)<<4);
            ring_publish(&ring,f);
        }
        int n = ring_window(&ring,window,sizeof window,first,RADIO_S3_BANK_WORDS,8,history,RADIO_S3_BANK_WORDS);
        assert(n > 0 && !ring.underruns);
        for (unsigned k=0; k<(unsigned)n+history; ++k)
            assert(window[k] == 16*((first/8-history+k)%8));
    }
    puts("counters: 500000 TS messages, seven sequence wraps and packed ring reads through 21.2 billion samples pass");
}
'''

MAIN = r'''
static void command(const char *text, bool accepted, bool solo) {
    char args[160]; snprintf(args,sizeof args,"%s",text);
    unsigned before = commands; cmd_dvb(args);
    assert(commands == before+(unsigned)accepted);
    if (accepted) assert(parsed_solo == solo);
}
int main(void) {
    command("2412 13 5m 540 0 128",true,false); assert(parsed_seconds == 540);
    command("2412 13 5m 61 17 508",true,false);
    command("2412 13 5m 541",false,false);
    command("2412 13 5m 0",false,false);
    command("2412 13 5m 4294967295",false,false);
    command("2412 13 5m 60 --phy-solo",true,true);
    command("2412 13 5m 61 --phy-solo",false,true);
    command("2389 13 5m 530",false,false);
    command("2451 13 5m 530",false,false);
    command("2412 13 5m 530 18",false,false);
    command("2412 13 5m 530 0 509",false,false);
    radio_s3_stats_t stats;
    radio_s3_options_t solo = {.phy_solo=true};
    const char *calls[] = {"N0CALL","A0000000000000000000000"};
    for (unsigned c=0; c<2; ++c) for (unsigned scale=1; scale<=2; ++scale) {
        call = calls[c];
        uint32_t rate = scale*RADIO_S3_DAC_RATE;
        for (uint32_t seconds=530; seconds<=540; ++seconds) {
            assert(!radio_s3_run_options(2412,0,seconds,rate,6750000,payload,NULL,NULL,&stats));
            assert(stats.id_count == 2 && stats.planned_samples > UINT32_MAX);
            if (!c && seconds == 530) assert(stats.planned_samples == (uint64_t)seconds*rate);
            if (!c && seconds == 540) assert(stats.planned_samples == (uint64_t)seconds*rate-observed.length);
        }
        unsigned before = tasks;
        assert(radio_s3_run_options(2412,0,541,rate,6750000,payload,NULL,NULL,&stats) == ESP_ERR_INVALID_ARG);
        assert(radio_s3_run_options(2412,0,61,rate,6750000,payload,NULL,&solo,&stats) == ESP_ERR_INVALID_ARG);
        assert(radio_s3_run_options(2389,0,530,rate,6750000,payload,NULL,NULL,&stats) == ESP_ERR_INVALID_ARG);
        assert(radio_s3_run_options(2451,0,530,rate,6750000,payload,NULL,NULL,&stats) == ESP_ERR_INVALID_ARG);
        assert(radio_s3_run_options(2412,18,530,rate,6750000,payload,NULL,NULL,&stats) == ESP_ERR_INVALID_ARG);
        assert(tasks == before);
        call = "N0CALL";
        assert(!radio_s3_run_options(2390,17,60,rate,6750000,payload,NULL,&solo,&stats));
        assert(!radio_s3_paced_run_rate(600,rate,payload,NULL,&stats));
        assert(stats.id_count == 3); // existing nine-minute paced ID is preserved
        assert(!radio_s3_paced_wifi_options(600,rate,payload,NULL,NULL,&stats));
        assert(stats.id_count == 3);
        assert(radio_s3_paced_wifi_options(61,rate,payload,NULL,&solo,&stats) == ESP_ERR_INVALID_ARG);
        // Execute every production bank deadline over 540 s, from an epoch
        // near wrap, with a synthetic clock. No radio/register implementation.
        memset(&stats,0,sizeof stats);
        stats.min_fill_slack_cycles = UINT32_MAX;
        run_t run = {.samples=540ULL*rate,.stats=&stats,.geo=geometry(rate)};
        clock_cycles = UINT32_MAX-1000;
        uint64_t epoch = clock_cycles;
        assert(!schedule(&run,(uint32_t)epoch,0,true));
        assert(stats.elapsed_cycles >= 540ULL*240000000);
        assert(stats.elapsed_cycles < 540ULL*240000000+100);
        assert(stats.elapsed_samples >= 540ULL*rate);
        assert(stats.switches == (run.samples-1)/run.geo.step);
        assert(!stats.fatal_late && clock_cycles-epoch == stats.elapsed_cycles+1);
    }
    call = "";
    unsigned before = tasks;
    assert(radio_s3_run_options(2412,0,530,RADIO_S3_DAC_RATE,6750000,payload,NULL,NULL,&stats) == ESP_ERR_INVALID_STATE);
    command("2412 13 5m 530",false,false);
    assert(tasks == before);
    counters();
    puts("duration: parser/API bounds, 530..540 s ID windows, paced IDs and 540 s cycle wraps at both DAC rates pass");
}
'''


if __name__ == "__main__":
    radio = (RADIO / "radio_s3.c").read_text()
    console = (ROOT / "firmware/main/main.c").read_text()
    geometry = radio[radio.index("typedef struct {"):radio.index("} geometry_t;")+len("} geometry_t;")]
    run_at = radio.index("typedef struct {\n    uint32_t freq_mhz;")
    run = radio[run_at:radio.index("} run_t;", run_at)+len("} run_t;")]
    source = MODEL + geometry + function(radio,"geometry") + run + WORKERS
    source += function(radio,"schedule") + function(radio,"on_core1") + function(radio,"known_rate")
    for name in ("radio_s3_run_options","radio_s3_paced_run_rate","radio_s3_paced_wifi_options"):
        source += function(radio,name)
    for name in ("number","tx_mode","take_option","cmd_dvb"):
        source += function(console,name)
    source += COUNTERS + MAIN
    build = ROOT / "firmware/build"
    build.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="radio-duration-",dir=build) as temp:
        folder = Path(temp)
        (folder / "esp_err.h").write_text("#pragma once\ntypedef int esp_err_t;\n")
        (folder / "esp_attr.h").write_text("#define IRAM_ATTR\n")
        (folder / "test.c").write_text(source)
        exe = folder / "test"
        subprocess.run(["cc","-std=c11","-D_POSIX_C_SOURCE=200809L","-O2","-Wall","-Wextra","-Werror",
                        "-I"+str(folder),"-I"+str(RADIO / "include"),"-I"+str(STATION / "include"),
                        "-I"+str(ROOT / "firmware/main"),"-I"+str(ROOT / "firmware/components/ingress/include"),
                        str(folder / "test.c"),str(STATION / "station_id.c"),str(ROOT / "firmware/main/ring.c"),
                        str(ROOT / "firmware/components/ingress/ingress.c"),
                        str(ROOT / "firmware/components/ingress/ingress_pattern.c"),"-lm","-o",str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
