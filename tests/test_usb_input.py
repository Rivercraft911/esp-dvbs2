"""Compile the actual USB adapter and PV parser against fragmented fake driver reads. No ports."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
NATIVE = r'''
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define ESP_OK 0
#define ESP_ERR_NO_MEM 1
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_SPIRAM 4
typedef int esp_err_t;
#define USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT 4
#define portMUX_INITIALIZER_UNLOCKED 0
typedef int portMUX_TYPE;
static bool rx_enabled=true;
static unsigned pauses, resumes;
static void portENTER_CRITICAL(portMUX_TYPE *mux) { (void)mux; }
static void portEXIT_CRITICAL(portMUX_TYPE *mux) { (void)mux; }
static void usb_serial_jtag_ll_ena_intr_mask(unsigned mask) {
    assert(mask==4); rx_enabled=true; ++resumes;
}
static void usb_serial_jtag_ll_disable_intr_mask(unsigned mask) {
    assert(mask==4); rx_enabled=false; ++pauses;
}
#define pdTRUE 1
#define pdMS_TO_TICKS(n) (n)
#define portMAX_DELAY UINT32_MAX
typedef void *TaskHandle_t;
typedef struct { void (*callback)(void *); const char *name; } esp_timer_create_args_t;
typedef void *esp_timer_handle_t;
static unsigned allocated, allocation_calls, fail_at, restarts, notifications;
static int64_t now;
static bool timer_active, timer_exists;
static void (*timer_callback)(void *);
static void *heap_caps_malloc(size_t n,int caps) {
    (void)caps;
    if (++allocation_calls==fail_at) return NULL;
    ++allocated; return malloc(n);
}
static void heap_caps_free(void *p) { if(p) { --allocated; free(p); } }
static int esp_timer_create(const esp_timer_create_args_t *cfg,esp_timer_handle_t *out) {
    if (++allocation_calls==fail_at) return ESP_ERR_NO_MEM;
    timer_callback=cfg->callback; *out=(void *)1; timer_exists=true; return 0;
}
static int esp_timer_start_once(esp_timer_handle_t t,int64_t us) {
    assert(t && !timer_active && us==50); timer_active=true; return 0;
}
static int esp_timer_stop_blocking(esp_timer_handle_t t,unsigned wait) {
    assert(t && wait==portMAX_DELAY); timer_active=false; return 0;
}
static void esp_timer_delete(esp_timer_handle_t t) { assert(t && !timer_active); timer_exists=false; }
static TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (void *)1; }
static void xTaskNotifyGive(TaskHandle_t t) { assert(t); ++notifications; }
static unsigned ulTaskNotifyTake(int clear,unsigned wait) {
    assert(clear==pdTRUE && wait<=1);
    if(wait && timer_active) { now+=50; timer_active=false; timer_callback(NULL); return 1; }
    return 0;
}
static uint8_t wire[1332*2048];
static bool paced_source;
static size_t acknowledged, credit_stalls;
static int64_t next_packet;
static size_t wire_len, wire_at, fragment, calls, queued;
static int64_t esp_timer_get_time(void) { return ++now; }
static void vTaskDelay(int ticks) { now+=ticks*1000; }
static int usb_serial_jtag_read_bytes(void *dst,size_t n,unsigned wait) {
    assert(wait==0 && !rx_enabled);
    if(n>queued) n=queued;
    memcpy(dst,wire+wire_at,n); wire_at+=n; queued-=n;
    return n;
}
static unsigned usb_serial_jtag_ll_read_rxfifo(uint8_t *dst,unsigned n) {
    assert(!rx_enabled); // packet edges never drive RX during the session
    if (paced_source) {
        // Host offers one USB packet per 111 us (~4.6 Mb/s). The hardware
        // FIFO holds two packets; a full FIFO NAKs rather than dropping bytes.
        while(next_packet<=now && acknowledged<wire_len) {
            const size_t packet=wire_len-acknowledged<64 ? wire_len-acknowledged : 64;
            if(acknowledged-wire_at+packet>128) { ++credit_stalls; next_packet=now; break; }
            acknowledged+=packet; next_packet+=111;
        }
        if(n>acknowledged-wire_at) n=acknowledged-wire_at;
    } else if (++calls%3==0) return 0;
    if(n>fragment) n=fragment;
    if(n>wire_len-wire_at) n=wire_len-wire_at;
    memcpy(dst,wire+wire_at,n); wire_at+=n;
    return n;
}
static void usb_serial_jtag_ll_clr_intsts_mask(unsigned mask) { assert(mask==4); }
static int usb_serial_jtag_wait_tx_done(unsigned wait) { assert(wait==500); return 0; }
static void esp_restart(void) { assert(!allocated && !timer_exists); ++restarts; rx_enabled=true; }

'''
APP = r'''
#include "usb_input.c"
static void append(unsigned type,unsigned seq,size_t n,int corrupt) {
    uint8_t *m=wire+wire_len;
    for(size_t i=0;i<n;++i) m[12+i]=(uint8_t)i;
    if(type==PV_TS) for(size_t i=0;i<n;i+=188) m[12+i]=0x47;
    pv_seal(m,type,seq,n);
    if(corrupt) m[123]^=1;
    wire_len+=1332;
}
int main(void) {
    for (fail_at=1; fail_at<=3; ++fail_at) {
        allocation_calls=0;
        assert(usb_input_start(PV_GCS)==ESP_ERR_NO_MEM && !allocated && !active);
        usb_input_stop(); assert(rx_enabled);
    }
    fail_at=0;
    const size_t fragments[]={1,13,64,1332};
    for(unsigned f=0;f<4;++f) {
        wire_len=wire_at=calls=0; fragment=fragments[f]; queued=37;
        append(PV_GCS,65534,1316,0);
        append(PV_GCS,65535,17,0);
        append(PV_GCS,0,19,1); // CRC rejected; forward gap counted
        append(PV_GCS,1,21,0);
        append(PV_GCS,1,21,0); // duplicate rejected
        append(PV_NOP,1,0,0); // backward NOP must not end the stream
        append(PV_TS,2,188,0); // wrong stream type rejected
        append(PV_NOP,3,0,0);
        assert(usb_input_start(PV_GCS)==ESP_OK && !rx_enabled);
        uint8_t got[2000]; size_t n=0;
        for(unsigned i=0;!ended && i<100000;++i) n+=usb_input_read(got+n,7);
        assert(ended && n==1354 && partial==0 && !rx_enabled);
        for(size_t i=0;i<1316;++i) assert(got[i]==(uint8_t)i);
        for(size_t i=0;i<17;++i) assert(got[1316+i]==(uint8_t)i);
        for(size_t i=0;i<21;++i) assert(got[1333+i]==(uint8_t)i);
        assert(ring.st.msgs==3 && ring.st.nops==1 && ring.st.bytes==n);
        assert(ring.st.err_crc==1 && ring.st.lost==1 && ring.st.seq_back==2 && ring.wrong_type==1);
        assert(ring.seq_next==4 && wire_at==wire_len);
        assert(driver_bytes==37 && fifo_bytes==wire_len-37 && notifications>0);
        usb_input_stop(); usb_input_print(); puts(""); usb_input_finish();
        assert(!allocated && !active && rx_enabled);
    }
    // Stop in the middle of a PV message. The command-task drain must finish
    // that message and discard the next one through NOP, without parsing text.
    wire_len=wire_at=calls=0; fragment=64;
    append(PV_GCS,0,1316,0); append(PV_GCS,1,1316,0); append(PV_NOP,2,0,0);
    assert(usb_input_start(PV_GCS)==ESP_OK);
    uint8_t byte;
    // Simulate a host disappearing after 17 bytes of a message. No NOP can
    // arrive: finish must emit diagnostics and restart, rather than quarantine
    // forever or return these bytes to the console parser.
    wire_len=17; queued=0;
    assert(usb_input_read(&byte,1)==0 && partial==17 && !rx_enabled);
    usb_input_stop(); usb_input_finish();
    assert(restarts==1 && !allocated && !active && rx_enabled && !timer_exists);
    // A fresh session after recovery is usable with no saved partial message.
    wire_len=wire_at=calls=0; fragment=64;
    append(PV_GCS,0,1316,0); append(PV_NOP,1,0,0);
    assert(usb_input_start(PV_GCS)==ESP_OK);
    uint8_t output[1317]; size_t got=0;
    while(!ended) got+=usb_input_read(output+got,sizeof output-got);
    assert(got==1316 && ring.st.msgs==1 && ring.st.nops==1 && partial==0);
    usb_input_stop(); usb_input_finish();
    assert(!allocated && !active && rx_enabled && !timer_exists && restarts==1);
    assert(pauses==6 && resumes==5); // once per session, never per packet/message
    wire_len=wire_at=queued=0; acknowledged=credit_stalls=0;
    paced_source=true; fragment=64; next_packet=now;
    for(unsigned i=0;i<2000;++i) append(PV_GCS,i,1316,0);
    append(PV_NOP,2000,0,0);
    assert(usb_input_start(PV_GCS)==ESP_OK);
    uint8_t frame[5370]; size_t pending=0, total=0;
    while(!ended) {
        const size_t n=usb_input_read(frame+pending,sizeof frame-pending);
        pending+=n; total+=n;
        if(pending==sizeof frame) { now+=2100; pending=0; } // PSRAM encoder burst on core 0
        else if(!n) vTaskDelay(1);
    }
    assert(total==2000*1316 && ring.st.msgs==2000 && ring.st.nops==1);
    assert(ring.st.err_crc==0 && ring.st.err_header==0 && ring.st.lost==0);
    assert(acknowledged==wire_len && wire_at==wire_len && credit_stalls>0);
    const double modeled=ring.st.bytes*8.0/(last-first);
    assert(modeled>3.5 && modeled<4.6);
    printf("USB finite FIFO + PSRAM encoder model: 2000 intact messages, %.3f Mb/s (simulation only)\n",modeled);
    usb_input_stop(); usb_input_finish();
    assert(!allocated && !active && rx_enabled && !timer_exists && restarts==1);
    puts("USB adapter: short/idle reads, stream splits, wrap, CRC, gaps, duplicates, type, FIFO ownership without RX edges, bounded restart recovery and allocation failures: ok");
}
'''


def main():
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp)
        (folder/'native.h').write_text(NATIVE)
        (folder/'app.c').write_text(APP)
        for name in ('esp_err.h', 'driver/usb_serial_jtag.h', 'esp_heap_caps.h',
                     'esp_timer.h', 'freertos/FreeRTOS.h', 'freertos/task.h', 'hal/usb_serial_jtag_ll.h', 'esp_system.h'):
            path = folder/name; path.parent.mkdir(exist_ok=True); path.write_text('#include "native.h"\n')
        ingress = ROOT/'firmware/components/ingress'
        exe = folder/'usb'
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-I'+tmp,
                        '-I'+str(ROOT/'firmware/main'), '-I'+str(ingress/'include'),
                        str(folder/'app.c'), str(ingress/'ingress.c'), str(ingress/'ingress_pattern.c'),
                        '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == '__main__':
    main()
