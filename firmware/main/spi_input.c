// General input and radio-off diagnostics. The second S3 runs this same firmware's spisend.
#include "spi_input.h"
#include "ingress_spi.h"
#include "ingress_pattern.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CPU_MHZ CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ
#define TX_BYTES (PV_MSG_BYTES + 64)
#ifdef CONFIG_SPI_INPUT_XIAO
static ingress_spi_config pins = {.sclk=7, .cs=1, .d0=2, .d1=4, .d2=5, .d3=6, .ready=8};
#else
static ingress_spi_config pins = {.sclk=12, .cs=10, .d0=11, .d1=13, .d2=14, .d3=9, .ready=8};
#endif
static unsigned input_type;
static bool loop;
static double target;
static unsigned fault;
static ingress_ring *rx;
static ingress_stats saved;
static ingress_spi_stats hw;
static uint32_t wrong_type, read_bytes, read_calls, read_max;
static uint64_t read_cycles;

static struct {
    spi_device_handle_t dev;
    TaskHandle_t task;
    bool bus;
    volatile bool stop, finished;
    unsigned type, fault, ms;
    double mbps;
    uint32_t sent, bytes, waits, forced, crc, errors;
    int khz;
    int64_t us;
    uint8_t *buf[2];
} tx;

unsigned spi_input_type(void) { return input_type; }

static void tx_free(void)
{
    if (tx.dev) spi_bus_remove_device(tx.dev);
    if (tx.bus) spi_bus_free(SPI3_HOST);
    heap_caps_free(tx.buf[0]); heap_caps_free(tx.buf[1]);
    tx.dev = NULL; tx.bus = false; tx.buf[0] = tx.buf[1] = NULL;
}

static esp_err_t tx_init(unsigned type, int mhz, double mbps, unsigned injected, unsigned ms)
{
    memset(&tx, 0, sizeof tx);
    tx.type = type; tx.mbps = mbps; tx.fault = injected; tx.ms = ms;
    spi_bus_config_t bus = {
        .data0_io_num=pins.d0, .data1_io_num=pins.d1, .data2_io_num=pins.d2,
        .data3_io_num=pins.d3, .sclk_io_num=pins.sclk,
        .data4_io_num=-1, .data5_io_num=-1, .data6_io_num=-1, .data7_io_num=-1,
        .max_transfer_sz=TX_BYTES, .flags=SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS,
    };
    const spi_device_interface_config_t cfg = {
        .command_bits=8, .address_bits=8, .dummy_bits=8, .mode=0,
        .clock_speed_hz=mhz*1000000, .spics_io_num=pins.cs,
        .flags=SPI_DEVICE_HALFDUPLEX, .queue_size=1,
        .cs_ena_pretrans=2, .cs_ena_posttrans=2,
    };
    esp_err_t err = spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) return err;
    tx.bus = true;
    err = spi_bus_add_device(SPI3_HOST, &cfg, &tx.dev);
    if (err != ESP_OK) goto fail;
    for (unsigned i=0; i<2; ++i) {
        tx.buf[i] = heap_caps_calloc(1, TX_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (!tx.buf[i]) { err = ESP_ERR_NO_MEM; goto fail; }
    }
    spi_device_get_actual_freq(tx.dev, &tx.khz);
    return ESP_OK;
fail:
    tx_free();
    return err;
}

static unsigned build(uint8_t *m, uint16_t seq)
{
    ingress_pattern(m+PV_HEADER_BYTES, seq, tx.type);
    pv_seal(m, tx.type, seq, PV_PAYLOAD_MAX);
    if ((seq & 63) != 63) return PV_MSG_BYTES;
    if (tx.fault == 1) m[PV_HEADER_BYTES+100] ^= 1;
    if (tx.fault == 2) return 1000;
    if (tx.fault == 3) return PV_MSG_BYTES+64;
    return PV_MSG_BYTES;
}

static void tx_run(void)
{
    const uint32_t period = tx.mbps > 0 ? PV_PAYLOAD_MAX*8.0/tx.mbps*CPU_MHZ : 0;
    bool waiting=false;
    uint16_t seq=0;
    unsigned cur=0, len=build(tx.buf[0], 0);
    uint32_t next=esp_cpu_get_cycle_count(), end=next-10*CPU_MHZ;
    const int64_t start=esp_timer_get_time();
    int64_t yielded=start;
    while (!tx.stop && (!tx.ms || esp_timer_get_time()-start < tx.ms*1000LL)) {
        const int64_t now=esp_timer_get_time();
        if (now-yielded >= 10000 || (tx.fault == 6 && (now-start)%100000 < 20000)) {
            vTaskDelay(1); yielded=now; next=esp_cpu_get_cycle_count();
            continue;
        }
        if (esp_cpu_get_cycle_count()-end < 10*CPU_MHZ ||
            (period && (int32_t)(esp_cpu_get_cycle_count()-next)<0)) continue;
        if (!gpio_get_level(pins.ready) && tx.fault != 4) {
            tx.waits += !waiting; waiting=true;
            next=esp_cpu_get_cycle_count(); continue;
        }
        waiting=false;
        tx.forced += !gpio_get_level(pins.ready);
        spi_transaction_t t = {.flags=SPI_TRANS_MODE_QIO, .cmd=0x23, .addr=0,
                                .tx_buffer=tx.buf[cur], .length=len*8};
        esp_err_t err=spi_device_polling_start(tx.dev, &t, portMAX_DELAY);
        if (err != ESP_OK) { tx.errors++; break; }
        const unsigned nextlen=build(tx.buf[cur^1], seq+1);
        err=spi_device_polling_end(tx.dev, portMAX_DELAY);
        if (err != ESP_OK) { tx.errors++; break; }
        end=esp_cpu_get_cycle_count(); next += period;
        // Diagnostic cost only: sender CRC covers bytes offered, including deliberate faults.
        tx.crc=pv_crc32(tx.crc, tx.buf[cur]+PV_HEADER_BYTES, PV_PAYLOAD_MAX);
        tx.sent++; tx.bytes += PV_PAYLOAD_MAX;
        seq++; cur^=1; len=nextlen;
        if (period && (int32_t)(end-next) > (int32_t)period) next=end;
    }
    tx.us=esp_timer_get_time()-start;
}

static void tx_task(void *arg)
{
    (void)arg;
    tx_run();
    tx.finished=true;
    vTaskSuspend(NULL);
}

static void tx_stop(void)
{
    if (tx.task) {
        tx.stop=true;
        while (!tx.finished) vTaskDelay(1);
        vTaskDelete(tx.task); tx.task=NULL;
    }
    tx_free();
}

static IRAM_ATTR bool received(void *ctx)
{
    BaseType_t woke=pdFALSE;
    vTaskNotifyGiveFromISR((TaskHandle_t)ctx, &woke);
    return woke==pdTRUE;
}

esp_err_t spi_input_start(void)
{
    memset(&saved, 0, sizeof saved); memset(&hw, 0, sizeof hw);
    read_bytes=read_calls=read_max=wrong_type=0; read_cycles=0;
    if (!input_type) return ESP_OK;
    esp_err_t err=ESP_OK;
    if (loop) err=tx_init(input_type, 40, target, fault, 0);
    if (err != ESP_OK) return err;
    ingress_spi_config cfg=pins; cfg.loopback=loop;
    cfg.received=received; cfg.ctx=xTaskGetCurrentTaskHandle();
    err=ingress_spi_start(&cfg, &rx);
    if (err != ESP_OK) { tx_free(); return err; }
    rx->type=input_type;
    if (loop && xTaskCreatePinnedToCore(tx_task, "spisend", 4096, NULL, 2, &tx.task, 1)!=pdPASS) {
        ingress_spi_stop(); rx=NULL; tx_free(); return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

size_t spi_input_read(uint8_t *dst, size_t n)
{
    const uint32_t before=esp_cpu_get_cycle_count();
    const size_t got=ingress_read(rx, dst, n);
    const uint32_t cycles=esp_cpu_get_cycle_count()-before;
    read_calls++; read_bytes+=got; read_cycles+=cycles;
    if (cycles>read_max) read_max=cycles;
    return got;
}

void spi_input_stop(void)
{
    if (!rx) return;
    if (loop) tx_stop();
    // Stop DMA/interrupts before taking the final counter snapshot.
    ingress_spi_stop(); ingress_spi_get_stats(&hw);
    saved=rx->st; wrong_type=rx->wrong_type; rx=NULL;
}

void spi_input_print(void)
{
    printf(",\"input\":\"%s\"", input_type==PV_TS ? "ts" : input_type==PV_GCS ? "gcs" : "synthetic");
    if (!input_type) return;
    printf(",\"input_fault\":%u,\"input_target\":%.3f", fault, target);
    printf(",\"input_loop\":%s,\"input_bytes\":%"PRIu32",\"input_calls\":%"PRIu32
           ",\"input_cycles\":%"PRIu64",\"input_call_max\":%"PRIu32,
           loop?"true":"false", read_bytes, read_calls, read_cycles, read_max);
    printf(",\"spi\":{\"msgs\":%"PRIu32",\"bytes\":%"PRIu32",\"nops\":%"PRIu32
           ",\"short\":%"PRIu32",\"long\":%"PRIu32",\"header\":%"PRIu32
           ",\"crc\":%"PRIu32",\"sync\":%"PRIu32",\"lost\":%"PRIu32
           ",\"back\":%"PRIu32",\"overruns\":%"PRIu32",\"restarts\":%"PRIu32
           ",\"ready_falls\":%"PRIu32",\"wrong_type\":%"PRIu32
           ",\"irqs\":%"PRIu32",\"isr_cycles\":%"PRIu32"}",
           saved.msgs, saved.bytes, saved.nops, saved.err_short, saved.err_long, saved.err_header,
           saved.err_crc, saved.err_sync, saved.lost, saved.seq_back, saved.overruns, saved.restarts,
           saved.ready_falls, wrong_type, hw.eof_irqs, hw.isr_cycles);
}

static unsigned kind(const char *s) { return !strcmp(s,"ts")?PV_TS:!strcmp(s,"gcs")?PV_GCS:0; }

static void probe(unsigned ms)
{
    const int64_t start=esp_timer_get_time();
    uint32_t crc=0, bad=0, messages=0, bytes=0, copy_max=0;
    uint8_t *want=heap_caps_malloc(2*PV_PAYLOAD_MAX, MALLOC_CAP_INTERNAL);
    if (!want) {
        spi_input_stop();
        printf("@{\"cmd\":\"spirx\",\"error\":\"probe allocation failed\"}\n");
        return;
    }
    uint8_t *copy=want+PV_PAYLOAD_MAX;
    while (esp_timer_get_time()-start < ms*1000LL) {
        if ((fault==4 || fault==5) && (esp_timer_get_time()-start)%50000<5000) {
            vTaskDelay(1); continue;
        }
        size_t n; unsigned type;
        const uint8_t *p=ingress_next(rx, &n, &type);
        if (!p) { ulTaskNotifyTake(pdTRUE, 1); continue; }
        const uint16_t seq=p[-8] | p[-7]<<8;
        const uint32_t before=esp_cpu_get_cycle_count();
        memcpy(copy, p, n);
        const uint32_t cycles=esp_cpu_get_cycle_count()-before;
        if (cycles>copy_max) copy_max=cycles;
        ingress_pattern(want, seq, type);
        bad += memcmp(copy, want, n)!=0;
        crc=pv_crc32(crc, copy, n); messages++; bytes+=n;
    }
    const int64_t us=esp_timer_get_time()-start;
    spi_input_stop();
    heap_caps_free(want);
    printf("@{\"cmd\":\"spirx\",\"us\":%lld,\"verified_msgs\":%"PRIu32
           ",\"verified_bytes\":%"PRIu32",\"bad_payload\":%"PRIu32",\"crc32\":%"PRIu32
           ",\"mbps\":%.3f,\"copy_max\":%"PRIu32,
           (long long)us, messages, bytes, bad, crc, bytes*8.0/us, copy_max);
    spi_input_print();
    if (loop) printf(",\"sent\":%"PRIu32",\"sender_errors\":%"PRIu32
                     ",\"sender_waits\":%"PRIu32,tx.sent,tx.errors,tx.waits);
    printf("}\n");
}

void spi_input_command(const char *cmd, const char *args)
{
    char a[16]="", b[16]="", extra;
    if (!strcmp(cmd,"spipins")) {
        ingress_spi_config cfg=pins;
        if (sscanf(args,"%15s %c",a,&extra)==1 && !strcmp(a,"xiao"))
            cfg=(ingress_spi_config){.sclk=7,.cs=1,.d0=2,.d1=4,.d2=5,.d3=6,.ready=8};
        else if (sscanf(args,"%15s %c",a,&extra)==1 && !strcmp(a,"devkit"))
            cfg=(ingress_spi_config){.sclk=12,.cs=10,.d0=11,.d1=13,.d2=14,.d3=9,.ready=8};
        else if (sscanf(args,"%d %d %d %d %d %d %d %c",&cfg.sclk,&cfg.cs,&cfg.d0,&cfg.d1,
                        &cfg.d2,&cfg.d3,&cfg.ready,&extra)!=7) goto usage;
        const int p[]={cfg.sclk,cfg.cs,cfg.d0,cfg.d1,cfg.d2,cfg.d3,cfg.ready};
        for (unsigned i=0;i<7;++i) {
            if (!GPIO_IS_VALID_OUTPUT_GPIO(p[i])) goto usage;
            for (unsigned j=0;j<i;++j) if (p[j]==p[i]) goto usage;
        }
        pins=cfg;
        printf("@{\"cmd\":\"spipins\",\"pins\":[%d,%d,%d,%d,%d,%d,%d]}\n",
               cfg.sclk,cfg.cs,cfg.d0,cfg.d1,cfg.d2,cfg.d3,cfg.ready); return;
    }
    if (!strcmp(cmd,"spiinput")) {
        double mbps=0; unsigned f=0;
        int n=sscanf(args,"%15s %15s %lf %u %c",a,b,&mbps,&f,&extra);
        if (n<1 || n>4 || (!kind(a)&&strcmp(a,"off")) ||
            (n>1 && strcmp(b,"loop")) || !isfinite(mbps) || mbps<0 || mbps>140 || f>6 ||
            (!strcmp(a,"off") && n!=1)) goto usage;
        input_type=kind(a); loop=n>1; target=mbps; fault=f;
        printf("@{\"cmd\":\"spiinput\",\"input\":\"%s\",\"loop\":%s,\"mbps\":%.3f,\"fault\":%u}\n",
               a,loop?"true":"false",target,fault); return;
    }
    unsigned ms=0, f=0; int mhz=40; double mbps=0;
    if (!strcmp(cmd,"spisend")) {
        int n=sscanf(args,"%u %15s %d %lf %u %c",&ms,a,&mhz,&mbps,&f,&extra);
        if (n<2 || n>5 || !kind(a) || ms<100 || ms>120000 || mhz<1 || mhz>40 ||
            !isfinite(mbps) || mbps<0 || mbps>140 || f>6) goto usage;
        gpio_set_direction(pins.ready, GPIO_MODE_INPUT); gpio_set_pull_mode(pins.ready, GPIO_PULLDOWN_ONLY);
        esp_err_t err=tx_init(kind(a),mhz,mbps,f,ms);
        if (err==ESP_OK) tx_run();
        printf("@{\"cmd\":\"spisend\",\"status\":\"%s\",\"sent\":%"PRIu32
               ",\"bytes\":%"PRIu32",\"crc32\":%"PRIu32",\"waits\":%"PRIu32
               ",\"forced\":%"PRIu32",\"errors\":%"PRIu32",\"sck_khz\":%d,\"us\":%lld%s}\n",
               esp_err_to_name(err),tx.sent,tx.bytes,tx.crc,tx.waits,tx.forced,tx.errors,tx.khz,(long long)tx.us, (err!=ESP_OK || tx.errors) ? ",\"error\":\"sender failed\"" : "");
        tx_free(); return;
    }
    if (!strcmp(cmd,"spirx")) {
        int n=sscanf(args,"%u %15s %15s %lf %u %c",&ms,a,b,&mbps,&f,&extra);
        if (n<2 || n>5 || !kind(a) || ms<100 || ms>120000 || (n>2 && strcmp(b,"loop")) ||
            !isfinite(mbps) || mbps<0 || mbps>140 || f>6) goto usage;
        const unsigned old=input_type, oldfault=fault; const bool oldloop=loop; const double oldtarget=target;
        input_type=kind(a); loop=n>2; target=mbps; fault=f;
        const esp_err_t err=spi_input_start();
        if (err==ESP_OK) probe(ms);
        else printf("@{\"cmd\":\"spirx\",\"error\":\"setup failed\",\"status\":\"%s\"}\n",esp_err_to_name(err));
        input_type=old; loop=oldloop; target=oldtarget; fault=oldfault; return;
    }
usage:
    printf("@{\"cmd\":\"%s\",\"error\":\"usage: spipins devkit|xiao|CLK CS D0 D1 D2 D3 READY; "
           "spiinput off|ts|gcs [loop [Mb/s [fault 0..6]]]; spisend ms ts|gcs [MHz [Mb/s [fault]]]; "
           "spirx ms ts|gcs [loop [Mb/s [fault]]]\"}\n",cmd);
}
