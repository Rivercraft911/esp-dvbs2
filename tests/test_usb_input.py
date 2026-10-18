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
static unsigned allocated, allocation_calls, fail_at;
static void *heap_caps_malloc(size_t n,int caps) {
    (void)caps;
    if (++allocation_calls==fail_at) return NULL;
    ++allocated; return malloc(n);
}
static void heap_caps_free(void *p) { if(p) { --allocated; free(p); } }
static uint8_t wire[1332*16];
static size_t wire_len, wire_at, fragment, calls;
static int64_t now;
static int64_t esp_timer_get_time(void) { return ++now; }
static void vTaskDelay(int ticks) { now+=ticks*1000; }
static int usb_serial_jtag_read_bytes(void *dst,size_t n,unsigned wait) {
    assert(wait<=1);
    if (!rx_enabled || wait) return 0;
    if (++calls%3==0) return 0; // intermittent USB idle
    if(n>fragment) n=fragment;
    if(n>wire_len-wire_at) n=wire_len-wire_at;
    memcpy(dst,wire+wire_at,n); wire_at+=n;
    return n;
}
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
    for (fail_at=1; fail_at<=2; ++fail_at) {
        allocation_calls=0;
        assert(usb_input_start(PV_GCS)==ESP_ERR_NO_MEM && !allocated && !active);
        usb_input_stop(); assert(rx_enabled);
    }
    fail_at=0;
    const size_t fragments[]={1,13,64,1332};
    for(unsigned f=0;f<4;++f) {
        wire_len=wire_at=calls=0; fragment=fragments[f];
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
        usb_input_stop(); usb_input_print(); puts(""); usb_input_finish();
        assert(!allocated && !active && rx_enabled);
    }
    // Stop in the middle of a PV message. The command-task drain must finish
    // that message and discard the next one through NOP, without parsing text.
    wire_len=wire_at=calls=0; fragment=64;
    append(PV_GCS,0,1316,0); append(PV_GCS,1,1316,0); append(PV_NOP,2,0,0);
    assert(usb_input_start(PV_GCS)==ESP_OK);
    uint8_t byte;
    assert(usb_input_read(&byte,1)==0 && rx_enabled); // unmask on an empty driver queue
    assert(usb_input_read(&byte,1)==0 && partial==128);
    usb_input_stop(); assert(!rx_enabled);
    usb_input_stop(); usb_input_finish();
    assert(wire_at==wire_len && !allocated && !active && rx_enabled);
    assert(pauses>32 && resumes>8);
    puts("USB adapter: short/idle reads, stream splits, wrap, CRC, gaps, duplicates, type, EOS/drain and allocation failures: ok");
}
'''


def main():
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp)
        (folder/'native.h').write_text(NATIVE)
        (folder/'app.c').write_text(APP)
        for name in ('esp_err.h', 'driver/usb_serial_jtag.h', 'esp_heap_caps.h',
                     'esp_timer.h', 'freertos/FreeRTOS.h', 'freertos/task.h', 'hal/usb_serial_jtag_ll.h'):
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
