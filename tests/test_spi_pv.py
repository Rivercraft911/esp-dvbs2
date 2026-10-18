"""Run the actual raw slave adapter against a queued SPI driver model. No hardware."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
NATIVE = r'''
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define IRAM_ATTR
#define ESP_OK 0
#define SPI2_HOST 2
#define SPI_DMA_CH_AUTO 1
#define SPI_SLAVE_NO_RETURN_RESULT 4
#define ESP_ERROR_CHECK(x) assert((x)==0)
#define portYIELD_FROM_ISR() (++yields)
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define portENTER_CRITICAL_ISR(p) ((void)(p))
#define portEXIT_CRITICAL_ISR(p) ((void)(p))
typedef int portMUX_TYPE;
typedef int esp_err_t;
typedef struct {
    int mosi_io_num,miso_io_num,sclk_io_num,quadwp_io_num,quadhd_io_num;
    int data4_io_num,data5_io_num,data6_io_num,data7_io_num,max_transfer_sz;
} spi_bus_config_t;
typedef struct { size_t length,trans_len; void *rx_buffer,*user; } spi_slave_transaction_t;
typedef struct {
    int spics_io_num,mode,queue_size,flags;
    void (*post_setup_cb)(spi_slave_transaction_t *),(*post_trans_cb)(spi_slave_transaction_t *);
} spi_slave_interface_config_t;
static spi_slave_interface_config_t callbacks;
static spi_slave_transaction_t *queue[4], *active;
static unsigned head,tail,yields,freed;
static int fail_init,fail_queue;
static uint32_t esp_cpu_get_cycle_count(void) { static uint32_t t; return ++t; }
static int spi_slave_initialize(int host,const spi_bus_config_t *b,
                               const spi_slave_interface_config_t *s,int dma) {
    assert(host==2 && dma==1 && b->max_transfer_sz==1336 && b->miso_io_num==-1);
    assert(b->mosi_io_num==-1 && b->sclk_io_num==-1 && s->spics_io_num==-1);
    assert(s->mode==0 && s->queue_size==4 && s->flags==4);
    head=tail=0; active=NULL; callbacks=*s; return fail_init;
}
static int spi_slave_queue_trans(int host,spi_slave_transaction_t *t,unsigned wait) {
    assert(host==2 && wait==0 && tail-head<4 && t->length==1336*8);
    if (fail_queue) return 1;
    queue[tail++%4]=t; return 0;
}
static int spi_slave_queue_reset(int host) { assert(host==2); head=tail; active=NULL; return 0; }
static int spi_slave_free(int host) { assert(host==2 && !active && head==tail); ++freed; return 0; }
static void arm(void) {
    assert(!active);
    if (head!=tail) { active=queue[head++%4]; callbacks.post_setup_cb(active); }
}
'''
APP = r'''
#include "ingress_spi_pv.c"
static ingress_ring ring;
static uint32_t buffers[4*1336/4];
static int pin;
static void ready(void *ctx,int level) {
    (void)ctx; pin=level && ingress_spi_pv_armed();
}
static void rearm(void *ctx,ingress_desc *slot,bool restart) {
    (void)ctx; assert(!restart); ingress_spi_pv_rearm(slot);
}
static bool received(void *ctx) { assert(ctx==&ring); return true; }
static const ingress_hw hw={.ready=ready,.rearm=rearm};
static void transfer(const uint8_t *m,size_t bits) {
    assert(active && pin);
    size_t n=(bits+7)/8; if(n>1336) n=1336;
    memcpy(active->rx_buffer,m,n);
    active->trans_len=bits>active->length?active->length:bits;
    callbacks.post_trans_cb(active); active=NULL;
    assert(!pin); // credit alone must never raise READY before setup callback
    arm();
}
int main(void) {
    ingress_spi_stats stats={0};
    ingress_spi_config cfg={.pv=true,.received=received,.ctx=&ring};
    ingress_init(&ring,(uint8_t *)buffers,&hw); ring.type=PV_TS;
    assert(ingress_spi_pv_start(&ring,&cfg,&stats)==0 && !pin);
    ingress_update_ready(&ring); assert(!pin); arm(); assert(pin);
    uint8_t msg[1400]={0},out[1316];
    msg[12]=0x47;
    uint32_t chain=0;
    for(unsigned seq=0;seq<70;++seq) {
        pv_seal(msg,PV_TS,seq,188);
        chain=pv_crc32(chain,msg+1328,4);
        transfer(msg,1332*8);
        assert(ingress_read(&ring,out,sizeof out)==188 && out[0]==0x47 && pin);
    }
    assert(ring.st.msgs==70 && ring.st.ts_packets==70 && ring.st.crc_chain==chain);
    // Short, non-byte-aligned, overlong and bad-CRC messages each cost one slot.
    const size_t sizes[]={1000*8,1332*8-1,1333*8,1400*8};
    for(unsigned i=0;i<4;++i) {
        transfer(msg,sizes[i]); assert(ingress_read(&ring,out,sizeof out)==0 && pin);
    }
    msg[200]^=1; transfer(msg,1332*8);
    assert(ingress_read(&ring,out,sizeof out)==0 && ring.st.err_crc==1);
    assert(ring.st.err_short==2 && ring.st.err_long==2);
    msg[200]^=1; pv_seal(msg,PV_NOP,70,0); transfer(msg,1332*8);
    assert(ingress_read(&ring,out,sizeof out)==0 && ring.st.nops==1);
    assert(ring.st.crc_chain==chain); // NOP must not enter the TS_DATA chain
    // Fill three slots: the two-slot READY margin withholds the last credit.
    for(unsigned seq=71;seq<74;++seq) {
        msg[12]=0x47; pv_seal(msg,PV_TS,seq,188); transfer(msg,1332*8);
    }
    assert(!pin && active);
    assert(ingress_read(&ring,out,sizeof out)==3*188 && pin);
    assert(stats.eof_irqs==79 && stats.isr_max && yields==79);
    ingress_spi_pv_stop(); assert(!ingress_spi_pv_armed() && freed==1);
    ingress_spi_pv_stop(); assert(freed==1);
    // The same raw driver and PV parser also carry arbitrary GCS bytes.
    ingress_init(&ring,(uint8_t *)buffers,&hw); ring.type=PV_GCS;
    assert(ingress_spi_pv_start(&ring,&cfg,&stats)==0);
    ingress_update_ready(&ring); arm(); assert(pin);
    for(unsigned seq=0;seq<2;++seq) {
        const size_t n=seq?1316:1;
        for(unsigned i=0;i<n;++i) msg[12+i]=(uint8_t)(i+seq);
        pv_seal(msg,PV_GCS,seq,n); transfer(msg,1332*8);
        assert(ingress_read(&ring,out,1)==1 && out[0]==seq);
        if (n>1) assert(ingress_read(&ring,out+1,n-1)==n-1);
        assert(memcmp(out,msg+12,n)==0 && pin);
    }
    assert(ring.st.msgs==2 && ring.st.bytes==1317 && ring.st.ts_packets==0);
    assert(ring.st.crc_chain==0 && ring.seq_next==2 && !ring.wrong_type);
    // An independent master fills the available slots while the encoder works.
    // A 5370-byte field exceeds all four 1316-byte payload slots combined: the
    // consumer must copy/release partial fields and retain a message's tail.
    uint8_t frame[5370];
    unsigned seq=2, offered=0, consumed=0;
    for (unsigned f=0;f<128;++f) {
        size_t used=0;
        while (used<sizeof frame) {
            while (pin) {
                for (unsigned i=0;i<1316;++i) msg[12+i]=(uint8_t)(offered+i);
                pv_seal(msg,PV_GCS,seq++,1316); transfer(msg,1332*8);
                offered+=1316;
            }
            const size_t n=ingress_read(&ring,frame+used,sizeof frame-used);
            assert(n>0); used+=n;
        }
        for (unsigned i=0;i<sizeof frame;++i) assert(frame[i]==(uint8_t)(consumed+i));
        consumed+=sizeof frame;
    }
    assert(ring.left && ring.held && ring.st.ready_falls>128);
    assert(!ring.st.overruns && !ring.st.err_crc && !ring.st.lost && !ring.wrong_type);
    ingress_spi_pv_stop(); assert(freed==2);
    fail_init=1; assert(ingress_spi_pv_start(&ring,&cfg,&stats)!=0);
    ingress_spi_pv_stop(); assert(freed==2);
    fail_init=0; fail_queue=1; assert(ingress_spi_pv_start(&ring,&cfg,&stats)!=0 && freed==3);
    puts("PV slave: real callbacks, DMA credit, CS rearm, lengths, CRC chain, NOP, backpressure, cleanup: ok");
}
'''


def main():
    component = ROOT/'firmware/components/ingress'
    with tempfile.TemporaryDirectory() as tmp:
        folder = pathlib.Path(tmp)
        (folder/'native.h').write_text(NATIVE)
        (folder/'app.c').write_text('#include <stdio.h>\n'+APP)
        for name in ('driver/spi_slave.h', 'esp_private/spi_slave_internal.h',
                     'esp_attr.h', 'esp_cpu.h', 'esp_err.h', 'freertos/FreeRTOS.h'):
            path = folder/name; path.parent.mkdir(exist_ok=True); path.write_text('#include "native.h"\n')
        exe = folder/'receiver'
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-I'+tmp, '-I'+str(component), '-I'+str(component/'include'),
                        str(folder/'app.c'), str(component/'ingress.c'),
                        str(component/'ingress_pattern.c'), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == '__main__':
    main()
