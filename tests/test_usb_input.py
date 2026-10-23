"""Compile the actual USB adapter and PV parser against fragmented fake driver reads. No ports."""
from pathlib import Path
import importlib.util
import json
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
static unsigned pauses, resumes, critical_depth;
static bool fifo_acknowledged;
static void portENTER_CRITICAL(portMUX_TYPE *mux) { (void)mux; assert(!critical_depth); ++critical_depth; }
static void portEXIT_CRITICAL(portMUX_TYPE *mux) { (void)mux; assert(critical_depth==1); --critical_depth; }
static unsigned usb_serial_jtag_ll_get_intr_ena_status(void) { return 8 | (rx_enabled?4:0); }
static void usb_serial_jtag_ll_ena_intr_mask(unsigned mask) {
    assert(mask==4 && critical_depth); rx_enabled=true; ++resumes;
}
static void usb_serial_jtag_ll_disable_intr_mask(unsigned mask) {
    assert(mask==4 && critical_depth); rx_enabled=false; ++pauses;
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
// Run memory (run_memory.h), counted and failed on request; the command that started the run
// returns it with run_reset (here: the test, after usb_input_finish).
static void *native_taken[16];
static unsigned native_takes;
static void *run_take(size_t n,size_t align,uint32_t caps) {
    (void)align; (void)caps;
    if (++allocation_calls==fail_at) return NULL;
    assert(native_takes<16); ++allocated; return native_taken[native_takes++]=calloc(1,n);
}
static void run_reset(void) { while (native_takes) { free(native_taken[--native_takes]); --allocated; } }
static int esp_timer_create(const esp_timer_create_args_t *cfg,esp_timer_handle_t *out) {
    if (++allocation_calls==fail_at) return ESP_ERR_NO_MEM;
    assert(!timer_exists); // created once, then kept
    timer_callback=cfg->callback; *out=(void *)1; timer_exists=true; return 0;
}
static int esp_timer_start_once(esp_timer_handle_t t,int64_t us) {
    assert(t && !timer_active && us==50); timer_active=true; return 0;
}
static int esp_timer_stop_blocking(esp_timer_handle_t t,unsigned wait) {
    assert(t && wait==portMAX_DELAY); timer_active=false; return 0;
}
static TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (void *)1; }
static void xTaskNotifyGive(TaskHandle_t t) { assert(t); ++notifications; }
static unsigned ulTaskNotifyTake(int clear,unsigned wait) {
    assert(clear==pdTRUE && wait<=1);
    if(wait && timer_active) { now+=50; timer_active=false; timer_callback(NULL); return 1; }
    return 0;
}
static uint8_t wire[1332*8192];
static bool paced_source;
static size_t acknowledged, credit_stalls;
static int64_t next_packet;
static int64_t host_timeout, host_progress;
static bool host_failed;
static size_t wire_len, wire_at, fragment, calls, queued;
static uint8_t driver_queue[8192];
static size_t steal_after, stolen, steal_size=1024;
static bool takeover_done;
static void preload_driver(size_t n) {
    assert(n<=sizeof driver_queue);
    memcpy(driver_queue,wire,n); queued=n; wire_at=n;
}
static void late_driver_isr(void) {
    // A delayed console-driver TX RMW restores its old RX-enable bit after the
    // handoff. While the encoder is not running, the RX ISR moves newer bytes
    // into its own software queue. The direct poller must not skip that queue.
    if(steal_after && !takeover_done && wire_at>=steal_after && !critical_depth) {
        rx_enabled=true; takeover_done=true;
        const size_t n=wire_len-wire_at<steal_size ? wire_len-wire_at : steal_size;
        assert(queued+n<=sizeof driver_queue);
        memcpy(driver_queue+queued,wire+wire_at,n); queued+=n; wire_at+=n; stolen+=n;
        now+=(int64_t)(n*8/4.6); // those USB bytes arrived while the ISR owned RX
        if(paced_source) { acknowledged=wire_at; next_packet=now+111; }
    }
}
static int64_t esp_timer_get_time(void) { ++now; late_driver_isr(); return now; }
static void vTaskDelay(int ticks) { now+=ticks*1000; }
static int usb_serial_jtag_read_bytes(void *dst,size_t n,unsigned wait) {
    assert(wait==0 && !rx_enabled && critical_depth);
    if(n>queued) n=queued;
    memcpy(dst,driver_queue,n); queued-=n;
    memmove(driver_queue,driver_queue+n,queued);
    return n;
}
static unsigned usb_serial_jtag_ll_read_rxfifo(uint8_t *dst,unsigned n) {
    assert(!rx_enabled && critical_depth && n<=64 && fifo_acknowledged);
    fifo_acknowledged=false;
    if (paced_source) {
        // Host offers one USB packet per 111 us (~4.6 Mb/s). The hardware
        // FIFO holds two packets; a full FIFO NAKs rather than dropping bytes.
        if(host_timeout && now-host_progress>=host_timeout) host_failed=true;
        while(!host_failed && next_packet<=now && acknowledged<wire_len) {
            const size_t packet=wire_len-acknowledged<64 ? wire_len-acknowledged : 64;
            if(acknowledged-wire_at+packet>128) { ++credit_stalls; next_packet=now; break; }
            acknowledged+=packet; next_packet+=111; host_progress=now;
        }
        if(n>acknowledged-wire_at) n=acknowledged-wire_at;
    } else if (++calls%3==0) return 0;
    if(n>fragment) n=fragment;
    if(n>wire_len-wire_at) n=wire_len-wire_at;
    memcpy(dst,wire+wire_at,n); wire_at+=n;
    return n;
}
static size_t usb_serial_jtag_get_read_bytes_available(void) { return queued; }
static void usb_serial_jtag_ll_clr_intsts_mask(unsigned mask) {
    assert(mask==4 && critical_depth); fifo_acknowledged=true;
}
static int usb_serial_jtag_wait_tx_done(unsigned wait) { assert(wait==500); return 0; }
static void esp_restart(void) { assert(!timer_active); ++restarts; rx_enabled=true; }

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
// Negative control: even with safe FIFO access, the previous "driver was empty
// once" strategy strands bytes after a late ISR refill. The fixed reader must
// reconstruct the same stream exactly from both queues, not just avoid a crash.
static size_t old_one_shot_read(uint8_t *dst,size_t n,bool *driver_was_empty) {
    portENTER_CRITICAL(&rx_mux);
    usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
    size_t got=0;
    if(!*driver_was_empty) {
        got=usb_serial_jtag_read_bytes(dst,n,0);
        if(!got) *driver_was_empty=true;
    }
    if(!got) {
        usb_serial_jtag_ll_clr_intsts_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
        got=usb_serial_jtag_ll_read_rxfifo(dst,n);
    }
    portEXIT_CRITICAL(&rx_mux);
    return got;
}
static void competing_readers(void) {
    for(unsigned fixed=0;fixed<2;++fixed) {
        wire_len=4096; wire_at=queued=calls=stolen=0; fragment=64; paced_source=false;
        steal_after=128; takeover_done=false; rx_enabled=false;
        for(size_t i=0;i<wire_len;++i) wire[i]=(i*17)^(i>>7);
        uint8_t got[4096]; size_t total=0; bool driver_was_empty=false;
        for(unsigned tries=0;tries<10000 && total<wire_len;++tries) {
            esp_timer_get_time(); // the console ISR can run between consumer calls
            const size_t n=fixed ? read_bytes(got+total,64) : old_one_shot_read(got+total,64,&driver_was_empty);
            total+=n;
            if(!n && wire_at==wire_len) break;
        }
        assert(takeover_done && stolen==1024);
        if(fixed) assert(total==wire_len && !memcmp(got,wire,wire_len) && queued==0);
        else assert(total==wire_len-1024 && queued==1024); // polling FIFO cannot make progress
    }
    steal_after=0; rx_enabled=true;
    puts("competing RX readers: old one-shot drain strands 1024 bytes; guarded reader recovers exact stream: ok");
}

// Run the actual blocking USB adapter with finite output slots, CW on core 1,
// and a host that abandons writes after no progress. The core-0 encoder cannot
// read more input while every output slot is occupied. Give both cores ideal
// scheduling: a 5 s host timeout STILL expires during the 5.22 s opening ID.
static void opening_id_backpressure(void) {
    for(unsigned ts=0;ts<2;++ts) for(unsigned fixed=0;fixed<2;++fixed) {
        wire_len=wire_at=queued=acknowledged=credit_stalls=0;
        paced_source=true; fragment=64; steal_after=0;
        const unsigned messages=ts?1834:7600;
        for(unsigned i=0;i<messages;++i) append(ts?PV_TS:PV_GCS,i,1316,0);
        append(PV_NOP,messages,0,0);
        assert(usb_input_start(ts?PV_TS:PV_GCS)==ESP_OK);
        const int64_t start=now;
        next_packet=host_progress=now; host_failed=false;
        host_timeout=fixed?25000000:5000000; // actual send.py policy for a 20 s run
        const unsigned slots=ts?8:2, bytes=ts?4016:5370;
        const int64_t frame_us=ts?33282:4439;
        int64_t next_play=start+100000+5220000;
        unsigned produced=0, consumed=0;
        uint8_t frame[5370]; size_t pending=0, total=0;
        uint32_t crc=0;
        while(!ended && !host_failed) {
            // Core 1 consumes one frame each frame interval, after CW. When
            // full, the encoder BLOCKS on the consumer's slot notification.
            while(produced>consumed && next_play<=now) { ++consumed; next_play+=frame_us; }
            if(produced-consumed==slots) {
                if(now<next_play) now=next_play;
                ++consumed; next_play+=frame_us;
            }
            const size_t n=usb_input_read(frame+pending,bytes-pending);
            crc=pv_crc32(crc,frame+pending,n); total+=n; pending+=n;
            if(pending==bytes) { now+=ts?3000:2100; pending=0; ++produced; }
            else if(!n) vTaskDelay(1);
        }
        if(fixed) {
            uint32_t expected=0;
            for(unsigned i=0;i<messages;++i) expected=pv_crc32(expected,wire+i*1332+12,1316);
            assert(!host_failed && ended && total==messages*1316 && crc==expected);
            assert(trace[6].reads && trace[6].payload_bytes); // live after the ID handoff
            for(unsigned second=1;second<5;++second) assert(!trace[second].reads); // full output ring
            assert(ring.st.err_crc==0 && ring.st.lost==0 && !queued);
            printf("CW scheduling model: %s, 25 s timeout survives ID, %u intact messages; reader active after ID: ok\n",ts?"TS":"GCS",messages);
        } else {
            assert(host_failed && !ended && now-start>=5220000 && total<40000);
            printf("CW scheduling negative control: %s, 5 s timeout abandons a healthy blocked reader before ID ends: reproduced\n",ts?"TS":"GCS");
            // Discarding an uncertain write must recover through restart.
            wire_len=wire_at; acknowledged=wire_at;
        }
        usb_input_stop(); usb_input_print(); puts(""); usb_input_finish(); run_reset();
        assert(!allocated && !active && rx_enabled && !timer_active);
        host_timeout=0; host_failed=false;
    }
}
int main(void) {
    competing_readers();

    for (fail_at=1; fail_at<=3; ++fail_at) {
        allocation_calls=0;
        assert(usb_input_start(PV_GCS)==ESP_ERR_NO_MEM && !active);
        usb_input_stop(); run_reset(); assert(rx_enabled && !allocated);
    }
    fail_at=0;
    const size_t fragments[]={1,13,64,1332};
    for(unsigned f=0;f<4;++f) {
        wire_len=wire_at=calls=0; fragment=fragments[f]; queued=0;
        append(PV_GCS,65534,1316,0);
        append(PV_GCS,65535,17,0);
        append(PV_GCS,0,19,1); // CRC rejected; forward gap counted
        append(PV_GCS,1,21,0);
        append(PV_GCS,1,21,0); // duplicate rejected
        append(PV_NOP,1,0,0); // backward NOP must not end the stream
        append(PV_TS,2,188,0); // wrong stream type rejected
        append(PV_NOP,3,0,0);
        preload_driver(37);
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
        usb_input_stop(); usb_input_print(); puts(""); usb_input_finish(); run_reset();
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
    usb_input_stop(); usb_input_finish(); run_reset();
    assert(restarts==1 && !allocated && !active && rx_enabled && !timer_active);
    // A fresh session after recovery is usable with no saved partial message.
    wire_len=wire_at=calls=0; fragment=64;
    append(PV_GCS,0,1316,0); append(PV_NOP,1,0,0);
    assert(usb_input_start(PV_GCS)==ESP_OK);
    uint8_t output[1317]; size_t got=0;
    while(!ended) got+=usb_input_read(output+got,sizeof output-got);
    assert(got==1316 && ring.st.msgs==1 && ring.st.nops==1 && partial==0);
    usb_input_stop(); usb_input_finish(); run_reset();
    assert(!allocated && !active && rx_enabled && !timer_active && restarts==1);
    assert(pauses>200 && resumes==5); // re-mask every read, restore once per session
    // Restore exactly the previous RX mask, preserving the console TX bit.
    wire_len=wire_at=queued=0; fragment=64; rx_enabled=false;
    append(PV_NOP,0,0,0);
    assert(usb_input_start(PV_GCS)==ESP_OK && !saved_rx_enabled);
    while(!ended) usb_input_read(&byte,1);
    usb_input_stop(); usb_input_finish();
    assert(!rx_enabled && usb_serial_jtag_ll_get_intr_ena_status()==8);
    rx_enabled=true;
    for(unsigned scenario=0;scenario<3;++scenario) {
        wire_len=wire_at=queued=0; acknowledged=credit_stalls=0;
        paced_source=true; fragment=64; next_packet=now;
        steal_after=scenario==1?14088:scenario==2?35964:0;
        steal_size=scenario==2?8192:1024;
        stolen=0; takeover_done=false;
        for(unsigned i=0;i<2000;++i) append(PV_GCS,i,1316,0);
        append(PV_NOP,2000,0,0);
        assert(usb_input_start(PV_GCS)==ESP_OK);
        uint8_t frame[5370]; size_t pending=0, total=0;
        uint32_t payload_crc=0;
        while(!ended) {
            const size_t n=usb_input_read(frame+pending,sizeof frame-pending);
            payload_crc=pv_crc32(payload_crc,frame+pending,n);
            pending+=n; total+=n;
            if(pending==sizeof frame) { now+=2100; pending=0; } // PSRAM encoder burst on core 0
            else if(!n) vTaskDelay(1);
        }
        uint32_t expected_crc=0;
        for(unsigned i=0;i<2000;++i) expected_crc=pv_crc32(expected_crc,wire+i*1332+12,1316);
        assert(total==2000*1316 && ring.st.msgs==2000 && ring.st.nops==1 && payload_crc==expected_crc);
        assert(ring.st.err_crc==0 && ring.st.err_header==0 && ring.st.lost==0);
        assert(acknowledged==wire_len && wire_at==wire_len && credit_stalls>0 && queued==0);
        assert(driver_bytes==stolen && fifo_bytes+driver_bytes==wire_len);
        assert(rx_reenabled==(scenario?1:0) && driver_queue_max==(scenario?steal_size:0));
        const double modeled=ring.st.bytes*8.0/(last-first);
        assert(modeled>3.5 && modeled<4.6);
        printf("USB finite FIFO + PSRAM model: 2000 intact messages, late driver bytes %zu, %.3f Mb/s (simulation only)\n",stolen,modeled);
        usb_input_stop(); usb_input_finish(); run_reset();
        assert(!allocated && !active && rx_enabled && !timer_active && restarts==1);
    }
    opening_id_backpressure();
    puts("USB adapter: short/idle reads, stream splits, wrap, CRC, gaps, duplicates, type, FIFO ownership without RX edges, bounded restart recovery and allocation failures: ok");
}
'''


def main():
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp)
        (folder/'native.h').write_text(NATIVE)
        (folder/'app.c').write_text(APP)
        for name in ('esp_err.h', 'driver/usb_serial_jtag.h', 'esp_heap_caps.h',
                     'esp_timer.h', 'driver/usb_serial_jtag_select.h', 'freertos/FreeRTOS.h', 'freertos/task.h', 'hal/usb_serial_jtag_ll.h', 'esp_system.h'):
            path = folder/name; path.parent.mkdir(exist_ok=True); path.write_text('#include "native.h"\n')
        ingress = ROOT/'firmware/components/ingress'
        exe = folder/'usb'
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-I'+tmp,
                        '-I'+str(ROOT/'firmware/tx/main'), '-I'+str(ingress/'include'),
                        '-I'+str(ROOT/'firmware/components/platform/include'),
                        str(folder/'app.c'), str(ingress/'ingress.c'), str(ingress/'ingress_pattern.c'),
                        '-o', str(exe)], check=True)
        output = subprocess.check_output([str(exe)], text=True, timeout=10)
        spec = importlib.util.spec_from_file_location('usb_send', ROOT/'examples/usb-send/send.py')
        send = importlib.util.module_from_spec(spec); spec.loader.exec_module(send)
        lines = output.splitlines()
        for i, line in enumerate(lines):
            if line.startswith('CW scheduling model:'):
                u = json.loads('{'+lines[i+1][1:]+'}')['usb']
                ts = 'TS,' in line
                row = dict(usb=u, status='ESP_OK', encoder_failed=False, frames_encoded=1,
                           underruns=0, late_fills=0, late_switches=0, fatal_late=0, min_fill_slack_cycles=1,
                           id_samples=208800000, dac_rate=40000000, seconds=20,
                           payload_bytes_per_frame=4016 if ts else 5370, symbol_rate=1000000 if ts else 5000000,
                           frame_symbols=33282 if ts else 22194)
                stats = dict(errors=[], terminal_nop=True, source_eof=True, messages=u['messages'],
                             payload_bytes=u['payload_bytes'], next_seq=u['messages'],
                             ts_packets=u['ts_packets'], crc_chain=u['crc_chain'])
                summary = send.check_run(stats, [row])
                assert summary['measurement_us'] >= 15000000
                assert summary['payload_mbps'] >= (.98*summary['mode_mbps'] if ts else 3.5)
                assert sum(t['payload_bytes'] for t in u['trace']) == u['payload_bytes']
                print(f"{'TS' if ts else 'GCS'} scheduling model: steady {summary['payload_mbps']:.3f} Mb/s over {summary['measurement_us']/1e6:.0f} s; wall rate {u['mbps']:.3f} Mb/s (simulation only)")
            if not line.startswith(('@', ',')):
                print(line)


if __name__ == '__main__':
    main()
