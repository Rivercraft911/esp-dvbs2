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
static inline int spi_bus_initialize(int h,const spi_bus_config_t *c,int dma) { (void)h;(void)c;(void)dma; return 0; }
static inline void spi_bus_free(int h) { (void)h; }
static inline int spi_bus_add_device(int h,const spi_device_interface_config_t *c,spi_device_handle_t *d) {
    (void)h; assert(c->mode==0 && c->dummy_bits==8); *d=(void *)1; return 0;
}
static inline void spi_bus_remove_device(spi_device_handle_t d) { (void)d; }
static inline void spi_device_get_actual_freq(spi_device_handle_t d,int *khz) { (void)d; *khz=40000; }
static inline const char *esp_err_to_name(int e) { return e ? "error" : "ESP_OK"; }
int spi_device_polling_start(spi_device_handle_t,spi_transaction_t *,unsigned);
static inline int spi_device_polling_end(spi_device_handle_t d,unsigned ticks) { (void)d;(void)ticks; now_us+=65; return 0; }
'''
APP = r'''
#include "spi_input.c"
int spi_device_polling_start(spi_device_handle_t d,spi_transaction_t *t,unsigned ticks) {
    (void)d;(void)ticks;
    assert(t->flags==SPI_TRANS_MODE_QIO && t->cmd==0x23 && t->addr==0 && t->length==1332*8);
    assert(pv_check(t->tx_buffer,1332)==PV_OK && now_us>=ready_at);
    const uint8_t *m=t->tx_buffer;
    assert((m[4] | m[5]<<8)==sequence++);
    if (!transfers) first_tx=now_us;
    else assert(now_us-last_tx >= (tx.mbps ? 470 : 75)); // rate ceiling or CS gap
    last_tx=now_us; ++transfers; return 0;
}
static ingress_ring receiver;
esp_err_t ingress_spi_start(const ingress_spi_config *cfg,ingress_ring **ring) {
    assert(cfg->received && cfg->ctx); *ring=&receiver; return ESP_OK;
}
void ingress_spi_stop(void) {}
void ingress_spi_get_stats(ingress_spi_stats *out) { memset(out,0,sizeof *out); }
int main(void) {
    for (unsigned test=0;test<3;++test) {
        now_us=alarms=delays=transfers=sequence=0; ready_at=test?2000:0;
        assert(tx_init(PV_TS,40,test==2?0:22,0,10)==ESP_OK);
        tx_run();
        assert(tx.errors==0 && transfers>14 && alarms>10 && first_tx>=ready_at);
        assert(first_tx < ready_at+100 && delays==0); // READY resumes within a poll, no 1 ms sleep
        tx_free(); assert(allocations==0 && !tx.timer);
    }
    input_type=PV_TS; loop=true;
    for (sender_core=0;sender_core<2;++sender_core) {
        assert(spi_input_start()==ESP_OK && tx.task);
        tx.finished=true; spi_input_stop();
        assert(!rx && !tx.task && allocations==0);
    }
    assert(tasks==2);
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
                        '-I'+str(ROOT/'firmware/main'),'-I'+str(ingress/'include'),
                        str(folder/'app.c'),str(ingress/'ingress.c'),str(ingress/'ingress_pattern.c'),
                        '-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=10)


if __name__ == '__main__': main()
