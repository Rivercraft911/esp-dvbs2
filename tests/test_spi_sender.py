"""Actual sender scheduler with a virtual timer/SPI device. No hardware or serial."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
NATIVE = r'''
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ 240
#define IRAM_ATTR
#define ESP_OK 0
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_INVALID_STATE 0x103
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_DMA 2
#define pdFALSE 0
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define configGENERATE_RUN_TIME_STATS 0
#define GPIO_IS_VALID_OUTPUT_GPIO(p) ((p)>=0 && (p)<49)
#define GPIO_MODE_INPUT 1
#define GPIO_PULLDOWN_ONLY 0
#define SPI3_HOST 3
#define SPI_DMA_CH_AUTO 0
#define SPICOMMON_BUSFLAG_MASTER 1
#define SPICOMMON_BUSFLAG_GPIO_PINS 2
#define SPI_DEVICE_HALFDUPLEX 1
#define SPI_TRANS_MODE_QIO 1
#define ESP_TIMER_TASK 0
#define eInvalid 0
typedef int esp_err_t;
typedef int BaseType_t;
typedef void *TaskHandle_t;
typedef void *spi_device_handle_t;
typedef struct { int data0_io_num,data1_io_num,data2_io_num,data3_io_num,sclk_io_num;
    int data4_io_num,data5_io_num,data6_io_num,data7_io_num,max_transfer_sz,flags; } spi_bus_config_t;
typedef struct { int command_bits,address_bits,dummy_bits,mode,clock_speed_hz,spics_io_num;
    int flags,queue_size,cs_ena_pretrans,cs_ena_posttrans; } spi_device_interface_config_t;
typedef struct { int flags,cmd,addr; const void *tx_buffer; size_t length; } spi_transaction_t;
typedef struct { void (*callback)(void *); const char *name; } esp_timer_create_args_t;
typedef struct { void (*callback)(void *); int64_t due; } timer_tst;
typedef timer_tst *esp_timer_handle_t;
static timer_tst timer;
static int64_t now_us, ready_at, first_tx, last_tx;
static unsigned alarms, delays, transfers;
static unsigned tasks;
static uint16_t sequence;
static int allocations;
static bool raw_wire;
static bool model_encoder;
static int64_t wire_finished;
static unsigned frame_bytes;
static inline int64_t esp_timer_get_time(void) { return now_us++; }
static inline uint32_t esp_cpu_get_cycle_count(void) { return now_us++*240; }
static inline int xPortGetCoreID(void) { return 0; }
static inline TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (void *)1; }
static inline void xTaskNotifyGive(TaskHandle_t t) { assert(t); }
static inline void vTaskNotifyGiveFromISR(TaskHandle_t t,BaseType_t *w) { (void)t; *w=0; }
static inline unsigned ulTaskNotifyTake(int clear,unsigned timeout) {
    (void)clear;
    if (timeout) { assert(timer.due>0); now_us=timer.due; timer.due=0; timer.callback(NULL); }
    return 0;
}
static inline void vTaskDelay(unsigned ticks) { ++delays; now_us+=1000*ticks; }
static inline void vTaskSuspend(TaskHandle_t t) { (void)t; }
static inline unsigned uxTaskGetStackHighWaterMark(TaskHandle_t t) { (void)t; return 1024; }
static inline void vTaskDelete(TaskHandle_t t) { (void)t; }
static inline int xTaskCreatePinnedToCore(void (*fn)(void *),const char *n,unsigned s,void *a,
    unsigned priority,TaskHandle_t *t,int core) {
    (void)fn;(void)n;(void)s;(void)a;(void)core;
    assert(priority < 5); // never preempt an active encoder, including inside memcpy
    ++tasks; *t=(void *)1; return pdPASS;
}
static inline int esp_timer_create(const esp_timer_create_args_t *a,esp_timer_handle_t *t) {
    timer.callback=a->callback; timer.due=0; *t=&timer; return 0;
}
static inline int esp_timer_start_once(esp_timer_handle_t t,int64_t wait) {
    assert(wait>0 && !t->due); t->due=now_us+wait; ++alarms; return 0;
}
static inline int esp_timer_stop_blocking(esp_timer_handle_t t,unsigned wait) { (void)wait; t->due=0; return 0; }
static inline void esp_timer_delete(esp_timer_handle_t t) { assert(!t->due); }
static inline int gpio_get_level(int pin) { (void)pin; return now_us>=ready_at; }
static inline void gpio_set_direction(int p,int v) { (void)p;(void)v; }
static inline void gpio_set_pull_mode(int p,int v) { (void)p;(void)v; }
static inline size_t heap_caps_get_free_size(int caps) { (void)caps; return 100000-allocations*1400; }
static inline void *heap_caps_malloc(size_t n,int caps) { (void)caps; ++allocations; return malloc(n); }
static inline void *heap_caps_calloc(size_t n,size_t m,int caps) { (void)caps; ++allocations; return calloc(n,m); }
static inline void heap_caps_free(void *p) { if(p) { --allocations; free(p); } }
// Run memory (run_memory.h): counted like the heap above, returned by run_reset.
static void *native_taken[64];
static unsigned native_takes;
static void *run_take(size_t n,size_t align,uint32_t caps) {
    (void)align; (void)caps; assert(native_takes<64);
    ++allocations; return native_taken[native_takes++]=calloc(1,n);
}
static void run_reset(void) { while (native_takes) { free(native_taken[--native_takes]); --allocations; } }
static inline int spi_bus_initialize(int h,const spi_bus_config_t *c,int dma) { (void)h;(void)c;(void)dma; return 0; }
static inline void spi_bus_free(int h) { (void)h; }
static inline int spi_bus_add_device(int h,const spi_device_interface_config_t *c,spi_device_handle_t *d) {
    (void)h; assert(c->mode==0); raw_wire=c->dummy_bits==0;
    assert(c->command_bits==(raw_wire?0:8) && c->address_bits==(raw_wire?0:8)); *d=(void *)1; return 0;
}
static inline void spi_bus_remove_device(spi_device_handle_t d) { (void)d; }
static inline void spi_device_get_actual_freq(spi_device_handle_t d,int *khz) { (void)d; *khz=40000; }
static inline const char *esp_err_to_name(int e) { return e ? "error" : "ESP_OK"; }
int spi_device_polling_start(spi_device_handle_t,spi_transaction_t *,unsigned);
static inline int spi_device_polling_end(spi_device_handle_t d,unsigned ticks) {
    (void)d;(void)ticks;
    if (!model_encoder) { now_us+=65; return 0; }
    // A raw 20 MHz transfer runs independently, but its completion wakes the
    // higher-priority consumer on this core before the sender returns from polling.
    if (now_us < wire_finished) now_us=wire_finished;
    now_us+=40; // per-message receive/CRC/copy work
    frame_bytes+=1316;
    if (frame_bytes>=5370) { frame_bytes-=5370; now_us+=2100; }
    now_us+=26; // diagnostic sender CRC
    return 0;
}
'''
APP = r'''
#include "spi_input.c"
void usb_input_control(bool (*control)(unsigned)) { (void)control; }
esp_err_t usb_input_start(unsigned type) { (void)type; return ESP_OK; }
size_t usb_input_read(uint8_t *dst,size_t n) { (void)dst; (void)n; return 0; }
void usb_input_stop(void) {}
void usb_input_print(void) {}
const ingress_ring *usb_input_ring(void) { return NULL; }
int spi_device_polling_start(spi_device_handle_t d,spi_transaction_t *t,unsigned ticks) {
    (void)d;(void)ticks;
    assert(t->flags==(raw_wire?0:SPI_TRANS_MODE_QIO) && t->cmd==(raw_wire?0:0x23) && t->addr==0 && t->length==1332*8);
    assert(pv_check(t->tx_buffer,1332)==PV_OK && now_us>=ready_at);
    {
        uint8_t want[1316];
        if (raw_wire && tx.type==PV_TS) ingress_pv_pattern(want,sequence);
        else ingress_pattern(want,sequence,tx.type);
        assert(memcmp(want,(const uint8_t *)t->tx_buffer+12,1316)==0);
    }
    const uint8_t *m=t->tx_buffer;
    assert(m[3]==tx.type && (m[4] | m[5]<<8)==sequence++);
    if (!transfers) first_tx=now_us;
    else assert(now_us-last_tx >= (tx.mbps ? 470 : 75)); // rate ceiling or CS gap
    if (model_encoder) {
        wire_finished=now_us+533; // 1332 bytes at 20 MHz
        now_us+=104; // GCS pattern/seal build, overlapped with the wire
    }
    last_tx=now_us; ++transfers; return 0;
}
// The receiver as ingress_spi.c provisions it: a session needs its wire's driver. A sender
// replays sealed slots, one per virtual millisecond, so spirx probes have messages to verify.
static ingress_ring receiver;
static int held=-1; // the provisioned driver: -1 none, 0 quad, 1 raw
static bool fail_provision;
esp_err_t ingress_spi_provision(bool raw) {
    if (fail_provision) { held=-1; return ESP_ERR_NO_MEM; }
    held=raw; return ESP_OK;
}
void ingress_spi_release(void) { held=-1; }
static void ready_stub(void *ctx,int level) { (void)ctx; (void)level; }
static void rearm_stub(void *ctx,ingress_desc *slot,bool restart) {
    (void)ctx; (void)restart; now_us+=1000; ingress_replay_release(&receiver,slot);
}
static const ingress_hw sender_hw={.rearm=rearm_stub,.ready=ready_stub};
esp_err_t ingress_spi_start(const ingress_spi_config *cfg,ingress_ring **ring) {
    assert(cfg->received && cfg->ctx);
    if (held != (cfg->pv ? 1 : 0)) return ESP_ERR_INVALID_STATE;
    pv_crc32_tables(cfg->crc);
    ingress_init(&receiver,NULL,&sender_hw);
    for (unsigned i=0;i<INGRESS_SLOTS;++i) receiver.desc[i].buf=cfg->buffers[i];
    receiver.type=PV_TS;
    if (!cfg->replay) ingress_replay_fill(&receiver);
    *ring=&receiver; return ESP_OK;
}
void ingress_spi_stop(void) { pv_crc32_tables(NULL); }
void ingress_spi_get_stats(ingress_spi_stats *out) { memset(out,0,sizeof *out); }
int main(void) {
    // A partially fitting borrowed pool must fall back to run memory;
    // two disjoint regions may supply the complete eight-slot pool.
    uint32_t pool[INGRESS_SLOTS*INGRESS_SLOT_BYTES/4];
    uint8_t *base=(uint8_t *)pool;
    spi_input_buffers(base,4*INGRESS_SLOT_BYTES,NULL,0);
    assert(!borrowed_count);
    spi_input_buffers(base,(INGRESS_SLOTS-1)*INGRESS_SLOT_BYTES,NULL,0);
    assert(!borrowed_count);
    spi_input_buffers(base,4*INGRESS_SLOT_BYTES,base+4*INGRESS_SLOT_BYTES,4*INGRESS_SLOT_BYTES);
    assert(borrowed_count==INGRESS_SLOTS);
    for (unsigned i=0;i<INGRESS_SLOTS;++i) assert(borrowed[i]==base+i*INGRESS_SLOT_BYTES);
    spi_input_buffers(NULL,0,NULL,0);
    for (unsigned test=0;test<12;++test) {
        pv_mode=test>=6;
        now_us=alarms=delays=transfers=sequence=0; ready_at=test?2000:0;
        assert(tx_init((test/3)%2?PV_GCS:PV_TS,pv_mode?20:40,test%3==2?0:22,10)==ESP_OK);
        tx_run();
        assert(tx.errors==0 && transfers>14 && alarms>10 && first_tx>=ready_at);
        assert(first_tx < ready_at+100 && delays==0); // READY resumes within a poll, no 1 ms sleep
        tx_free(); run_reset(); assert(allocations==0 && !tx.timer);
    }
    pv_mode=false; input_type=PV_TS;
    assert(spi_input_start()==ESP_ERR_INVALID_STATE); run_reset(); // no driver provisioned
    spi_input_command("spiinput","quad");
    assert(spi_input_start()==ESP_OK); spi_input_stop(); run_reset();
    assert(!rx && allocations==(crc_table!=NULL) && !tasks); // the wire keeps its CRC table
    // spirx borrows the other wire's driver, then gives the selection its own back, after a
    // probe and after a failed setup, so the next encoder start finds it.
    const char *probes[][2]={{"quad ts replay","100 on"},{"on gcs","100 quad"},{"quad","100 quad"},
                             {"off","100 on"},{"usb","100 quad"}};
    for (unsigned i=0;i<5;++i) {
        spi_input_command("spiinput",probes[i][0]);
        const int before=held;
        const unsigned type=input_type; const bool pv=pv_mode, rep=replay, usb=usb_mode;
        spi_input_command("spirx",probes[i][1]);
        assert(held==before && input_type==type && pv_mode==pv && replay==rep && usb_mode==usb);
        run_reset(); assert(!rx && allocations==(crc_table!=NULL)); // the probe's memory comes back
        if (type && !usb) { assert(spi_input_start()==ESP_OK); spi_input_stop(); run_reset(); }
    }
    spi_input_command("spiinput","quad ts replay");
    fail_provision=true;
    spi_input_command("spirx","100 on"); // setup fails; so does the restore, reported
    fail_provision=false;
    assert(held==-1);
    spi_input_command("spirx","100 on");
    run_reset();
    assert(held==0 && spi_input_start()==ESP_OK); spi_input_stop(); run_reset();
    assert(!rx && allocations==(crc_table!=NULL));
    spi_input_command("spiinput","on");
    assert(pv_mode && !replay && input_type==PV_TS && held==1);
    spi_input_command("spiinput","on replay"); // replay is quad only: rejected without changes
    assert(pv_mode && !replay && input_type==PV_TS);
    const char *invalid[]={"on loop 9","on garbage","quad ts extra","off ts","quad replay 9","quad gcs loop 22"};
    for (unsigned i=0;i<sizeof invalid/sizeof *invalid;++i) {
        spi_input_command("spiinput",invalid[i]);
        assert(pv_mode && !replay && input_type==PV_TS);
    }
    spi_input_command("spiinput","on gcs");
    assert(pv_mode && !replay && input_type==PV_GCS);
    spi_input_command("spiinput","quad ts replay");
    assert(!pv_mode && replay && input_type==PV_TS && held==0);
    spi_input_command("spiinput","quad");
    assert(!pv_mode && !replay && input_type==PV_TS);
    spi_input_command("spiinput","pv"); // compatible old raw selector
    assert(pv_mode && input_type==PV_TS);
    spi_input_command("spiinput","gcs"); // compatible old quad selector
    assert(!pv_mode && input_type==PV_GCS);
    spi_input_command("spiinput","ts");
    assert(!pv_mode && input_type==PV_TS);
    // The selected wire keeps its CRC table (one heap block) until spiinput off.
    assert(crc_table && allocations == 1);
    spi_input_command("spisend","100 on gcs 21"); // no raw clock above 20 MHz
    assert(!pv_mode && allocations == 1);
    spi_input_command("spisend","100 quad 40 22 1"); // the fault argument is retired
    assert(!pv_mode && allocations == 1);
    spi_input_command("spirx","100 quad loop 22"); // so is the on-chip loopback
    assert(!pv_mode && allocations == 1);
    spi_input_command("spipins","xiao");
    assert(pins.sclk==7 && pins.cs==1 && pins.d0==2 && pins.ready==8);
    spi_input_command("spipins","devkit");
    assert(pins.sclk==12 && pins.cs==10 && pins.d0==11 && pins.ready==8);
    spi_input_command("spiinput","usb gcs"); // USB input needs neither SPI driver nor CRC table
    assert(usb_mode && input_type==PV_GCS && !replay && held==-1 && !crc_table && !allocations);
    spi_input_command("spiinput","usb ts extra");
    assert(usb_mode && input_type==PV_GCS);
    spi_input_command("spiinput","usb");
    assert(usb_mode && input_type==PV_TS);
    spi_input_command("spiinput","on gcs");
    assert(!usb_mode && input_type==PV_GCS);
    assert(spi_input_start()==ESP_OK && native_takes==1); // slots only: the table is provisioned
    // While the session runs, no selection may change or free its driver and CRC table.
    uint32_t *live=crc_table;
    const char *during[][2]={{"spiinput","off"},{"spiinput","usb"},{"spiinput","quad ts"},{"spirx","100 on"}};
    for (unsigned i=0;i<4;++i) {
        spi_input_command(during[i][0],during[i][1]);
        assert(crc_table==live && held==1 && pv_mode && !usb_mode && input_type==PV_GCS && allocations==2);
    }
    spi_input_stop(); run_reset();
    spi_input_command("spiinput","off");
    assert(!pv_mode && !replay && !input_type && held==-1 && !crc_table && !allocations);
    puts("SPI sender: real scheduler, timer sleeps, 22 Mb/s pacing, fresh READY, CRC and cleanup: ok");
}
'''


def main():
    with tempfile.TemporaryDirectory() as tmp:
        folder=pathlib.Path(tmp)
        (folder/'native.h').write_text(NATIVE)
        (folder/'app.c').write_text(APP)
        for name in ('esp_err.h','driver/gpio.h','driver/spi_master.h','esp_cpu.h',
                     'esp_heap_caps.h','esp_timer.h','freertos/FreeRTOS.h','freertos/task.h'):
            p=folder/name; p.parent.mkdir(exist_ok=True); p.write_text('#include "native.h"\n')
        ingress=ROOT/'firmware/components/ingress'
        exe=folder/'sender'
        subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-I'+tmp,
                        '-I'+str(ROOT/'firmware/tx/main'),'-I'+str(ingress/'include'),
                        '-I'+str(ROOT/'firmware/components/platform/include'),
                        str(folder/'app.c'),str(ingress/'ingress.c'),str(ingress/'ingress_pattern.c'),
                        '-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=10)


if __name__ == '__main__':
    main()
    from test_spi_pv import main as receiver_checks
    receiver_checks()
