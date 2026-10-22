/* Finite collector with three raw banks, one 28,512-byte workspace and
 * bounded interrupt protection around shared PIE state. DMA completion
 * transfers ownership; capture indices establish sample continuity. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "collector.h"

#include "dma.h"

#include "rx_halfband.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"

#include "esp_cpu.h"
#include "esp_private/esp_clk.h"
#include "esp_rom_crc.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_private/regi2c_ctrl.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_freertos_hooks.h"
#include "heap_memory_layout.h"
#include "soc/soc.h"

#define DUMP_CTRL 0x60033d5cU
#define DUMP_INDEX 0x60033d60U
#define DAC_CTRL 0x60033d64U
#define DUMP_LANES 0x60033d90U
#define BANK_USAGE 0x600c101cU
#define AGC_FORCE 0x6001c02cU
#define FE_CTRL 0x60006000U
#define UNIT_PAIRS RX_UNIT_PAIRS
#define LATE_PAIRS RX_LATE_PAIRS
#define PAIR_CYCLES 15U
#define CONTROL_PAIRS 512U

SOC_RESERVE_MEMORY_REGION(0x3fcb0000, 0x3fce0000, radio_rx_banks);

extern void stop_tx_tone(unsigned int enable);
extern void force_txon_mode(uint8_t enable, uint8_t mode, uint8_t offset);
extern void rom_pbus_workmode(void);
extern void rom_pbus_xpd_tx_off(void);
extern void rom_pbus_xpd_rx_on(unsigned int enable);
extern void rom_set_rxclk_en(unsigned int enable);
extern void set_chanfreq(uint16_t mhz, uint8_t bandwidth);
extern void phy_set_freq(uint16_t mhz, int16_t khz);
extern void force_rx_gain(uint8_t force, uint8_t index, uint8_t bluetooth);

struct stream_state;
typedef struct {
    uint32_t *snapshot;rx_report *report;uint16_t mhz;uint8_t gain;
    struct stream_state *stream;rx_stream_report *stream_report;
    rx_spill *spill;SemaphoreHandle_t done;
    uint8_t *stage;hb_tile_state *filter_state;
} run;
static volatile uint32_t core1_ticks;
static void IRAM_ATTR count_core1_tick(void)
{
    ++core1_ticks;
}
static uint32_t *IRAM_ATTR bank(unsigned n)
{
    return (uint32_t *)(0x3fcb0000U + n * 65536U);
}
static bool lo_select(bool divided, uint8_t *actual)
{
    uint8_t before = regi2c_ctrl_read_reg(0x65, 0, 0);
    uint8_t expected = rx_lo_value(before, divided);
    if (expected != before) regi2c_ctrl_write_reg_mask(0x65, 0, 0, 4, 4, divided);
    *actual = regi2c_ctrl_read_reg(0x65, 0, 0);
    return *actual == expected;
}
static void IRAM_ATTR clear_bank(unsigned n)
{rx_halfband_clear(bank(n));}

enum {FILTER_STAGE=7360U,FILTER_WORKSPACE=28512U,FILTER_BUDGET=154335U};
_Static_assert(2U*FILTER_STAGE+sizeof(hb_vector_scratch)+128U==FILTER_WORKSPACE,"workspace geometry");
_Static_assert(sizeof(hb_tile_state)==96U,"filter state");
static bool IRAM_ATTR filter_guards(run *r)
{
    uint8_t *scratch = r->stage + 2U * FILTER_STAGE + 64U;
    for (unsigned n = 0; n < 64U; ++n)
        if (scratch[(int)n - 64] != 0x6d || scratch[sizeof(hb_vector_scratch) + n] != 0x6d)
            return false;
    return true;
}

static volatile uint32_t idle_visits;
static bool registered;
static bool idle_return(void)
{
    ++idle_visits;
    taskYIELD();
    return false;
}
bool ch_idle_open(void)
{
    if(registered||xPortGetCoreID()!=1||uxTaskPriorityGet(NULL)!=tskIDLE_PRIORITY)return false;
    idle_visits=0;
    if(esp_register_freertos_idle_hook_for_cpu(idle_return,1)!=ESP_OK)return false;
    registered=true;return true;
}
void ch_idle_close(void)
{
    if(registered){esp_deregister_freertos_idle_hook_for_cpu(idle_return,1);registered=false;}
}
uint32_t CH_HOT ch_idle_count(void){return idle_visits;}
typedef struct {
    rrc4_tile_partial filter;
    ch_lease lease;
    uint64_t native_pairs, matched_pairs;
    unsigned consumed;
    bool active;
} rrc4_chunk_batch;
typedef struct {
    uint64_t matched_first, filtered_window_first;
    unsigned input_pairs, output_pairs;
    bool chunk_done;
} rrc4_chunk_batch_result;
_Static_assert(sizeof(rrc4_chunk_batch)==4816,"bounded tile plus borrowed-lease cursor");
_Static_assert(_Alignof(rrc4_chunk_batch)==16,"same internal-state alignment");

bool rrc4_chunk_batch_init(rrc4_chunk_batch *s,unsigned phase)
{
    if(!s || (uintptr_t)s%16 || phase>3)return false;
    memset(s,0,sizeof *s);
    return rrc4_tile_partial_init(&s->filter,phase);
}

bool rrc4_chunk_batch_begin(rrc4_chunk_batch *s,const ch_lease *lease)
{
    if(!s || (uintptr_t)s%16 || !lease || !lease->data || s->active ||
       lease->meta.input_pairs<CH_UNIT-CH_LATE || lease->meta.input_pairs>CH_UNIT+CH_LATE ||
       lease->meta.input_first!=s->native_pairs || lease->meta.output_first!=s->filter.total ||
       s->native_pairs>UINT64_MAX-lease->meta.input_pairs)return false;
    uint64_t native_end=s->native_pairs+lease->meta.input_pairs;
    uint64_t filtered_end=(native_end-27U)/4U;
    if(filtered_end<s->filter.total || filtered_end-s->filter.total!=lease->meta.output_pairs ||
       !lease->meta.output_pairs)return false;
    s->lease=*lease;s->consumed=0;s->active=true;
    return true;
}

bool CH_HOT rrc4_chunk_batch_step(rrc4_chunk_batch *s,int16_t *out,size_t capacity,
                     rrc4_chunk_batch_result *result,bool vector)
{
    if(!s || (uintptr_t)s%16 || !result || !s->active ||
       s->consumed>=s->lease.meta.output_pairs ||
       s->filter.total!=s->lease.meta.output_first+s->consumed ||
       s->matched_pairs>(UINT64_MAX-s->filter.phase)/4U)return false;
    unsigned pairs=s->lease.meta.output_pairs-s->consumed;
    if(pairs>RRC4_TILE_PAIRS)pairs=RRC4_TILE_PAIRS;
    uint64_t first=s->matched_pairs;size_t made;
    /* No sink, yield, queue release or callback inside the caller's mask. */
    if(!rrc4_tile_partial_push(&s->filter,(const int8_t *)s->lease.data+2U*s->consumed,
                       pairs,out,capacity,&made,vector))return false;
    s->consumed+=pairs;s->matched_pairs+=made;
    bool done=s->consumed==s->lease.meta.output_pairs;
    if(done){s->native_pairs=s->lease.meta.input_first+s->lease.meta.input_pairs;s->active=false;}
    *result=(rrc4_chunk_batch_result){first,made?s->filter.phase+4U*first:0,pairs,(unsigned)made,done};
    return true;
}

/* Stream adapter uses the same bank/filter/spill ownership helpers.
 * No captured-record CRC, demodulator or FEC callback executes here. */
#include "esp_timer.h"

bool ch_idle_open(void);void ch_idle_close(void);uint32_t ch_idle_count(void);
typedef struct stream_state {
    ch_queue queue;
    ch_pipe pipe;
    run *owner;
    TaskHandle_t producer;
    uint8_t *arena;
    bool reader_started,idle_registered;
    unsigned bank;uint16_t first;
    uint32_t ownership,epoch,idle_start;
    stream_epoch clock;
    uint8_t *rrc_memory;
    rrc4_chunk_batch *rrc;
    int16_t *rrc_output;
} stream_state;

enum { RRC_OUTPUT_PAIRS=128U, RRC_OUTPUT_BYTES=512U, RRC_PREFIX_PAIRS=128U,
       RRC_MEMORY_BYTES=64U+sizeof(rrc4_chunk_batch)+64U+RRC_OUTPUT_BYTES+64U };
_Static_assert(sizeof(rrc4_chunk_batch)==4816U && RRC_MEMORY_BYTES==5520U,"boundedRRCworkspace");
/* Core1 owns the clock and checkpoints. No clock or console work runs in an ISR. */
static bool IRAM_ATTR stream_time_add(run *r,uint32_t now,uint32_t ticks)
{
    stream_state *v=r->stream;rx_stream_report *s=r->stream_report;
    uint32_t gap=ticks-v->clock.ticks;
    if(gap>s->max_epoch_tick_gap)s->max_epoch_tick_gap=gap;
    bool ok=stream_epoch_add(&v->clock,now,ticks);
    if(!ok&&s->epoch_valid)++s->epoch_errors;
    s->epoch_valid=ok;return ok;
}
static void IRAM_ATTR stream_checkpoint_save(run *r,bool sample_clock)
{
    stream_state *v=r->stream;rx_stream_report *s=r->stream_report;
    uint32_t units=r->report->units;
    if(!units||s->checkpoint_count>=STREAM_CHECKPOINTS||
       (s->checkpoint_count&&s->checkpoint[s->checkpoint_count-1U].units>=units))return;
    if(sample_clock) {
        uint32_t now=esp_cpu_get_cycle_count();
        uint32_t ticks=core1_ticks;
        (void)stream_time_add(r,now,ticks);
    }
    uint32_t released=atomic_load_explicit(&v->queue.released,memory_order_acquire);
    uint32_t published=atomic_load_explicit(&v->queue.published,memory_order_relaxed);
    unsigned queued=v->queue.reserved-released;
    if(queued>s->interval_max_queued)s->interval_max_queued=queued;
    s->checkpoint[s->checkpoint_count++]=(stream_checkpoint){
        .elapsed_cycles=s->epoch_valid?v->clock.total:0,.units=units,
        .retained_pairs=r->report->retained_pairs,.reserved=v->queue.reserved,
        .published=published,.released=released,.queued=queued,
        .interval_max_queued=s->interval_max_queued,.epoch_valid=s->epoch_valid};
    s->interval_max_queued=0;
}
static bool stream_rrc_guards(const stream_state *v)
{
    const uint8_t *p=v->rrc_memory;
    for(unsigned n=0;n<64;++n)
        if(p[n]!=0x6d||p[64U+sizeof(rrc4_chunk_batch)+n]!=0x6d||p[RRC_MEMORY_BYTES-64U+n]!=0x6d)return false;
    return true;
}
static bool matched_prefix_guards(const rx_stream_report *s)
{
    if(!s->matched_allocation||s->matched_prefix_pairs>RRC_PREFIX_PAIRS)return false;
    for(unsigned n=0;n<64;++n)
        if(s->matched_allocation[n]!=0x6d||s->matched_prefix[RRC_OUTPUT_BYTES+n]!=0x6d)return false;
    for(unsigned n=4U*s->matched_prefix_pairs;n<RRC_OUTPUT_BYTES;++n)if(s->matched_prefix[n]!=0x6d)return false;
    return true;
}
/* No vector value, callback, CRC or progress work crosses this bounded mask. */
static bool IRAM_ATTR __attribute__((noinline)) stream_rrc_step(stream_state *v,rrc4_chunk_batch_result *result)
{
    uint32_t begin=esp_cpu_get_cycle_count(),before,after;
    uint32_t irq=portSET_INTERRUPT_MASK_FROM_ISR();
    __asm__ volatile("rsr %0,cpenable":"=a"(before));
    bool ok=rrc4_chunk_batch_step(v->rrc,v->rrc_output,RRC_OUTPUT_PAIRS,result,true);
    __asm__ volatile("rsr %0,cpenable":"=a"(after));
    portCLEAR_INTERRUPT_MASK_FROM_ISR(irq);
    uint32_t elapsed=esp_cpu_get_cycle_count()-begin;
    rx_stream_report *s=v->owner->stream_report;
    if(elapsed>s->max_rrc_mask_cycles)s->max_rrc_mask_cycles=elapsed;
    s->rrc_context_ok=s->rrc_context_ok&&before==after;
    return ok&&before==after;
}
/* Preflight uses the same core 0 task and guarded buffers before ADC starts. */
static bool IRAM_ATTR __attribute__((noinline)) stream_partial_push(void *opaque,const int8_t *input,
                                                                  size_t pairs,size_t *made)
{
    stream_state *v=opaque;uint32_t before,after;
    uint32_t irq=portSET_INTERRUPT_MASK_FROM_ISR();
    __asm__ volatile("rsr %0,cpenable":"=a"(before));
    bool ok=rrc4_tile_partial_push(&v->rrc->filter,input,pairs,v->rrc_output,RRC_OUTPUT_PAIRS,made,true);
    __asm__ volatile("rsr %0,cpenable":"=a"(after));
    __asm__ volatile("wsr %0,cpenable;rsync"::"a"(before):"memory");
    portCLEAR_INTERRUPT_MASK_FROM_ISR(irq);
    return ok&&before==after;
}
static bool stream_partial_guards(void *opaque)
{
    stream_state *v=opaque;
    const rx_stream_report *s=v->owner->stream_report;
    const uint8_t *p=(const uint8_t *)v->rrc;
    for(unsigned n=sizeof v->rrc->filter;n<sizeof *v->rrc;++n)if(p[n]!=0x6d)return false;
    for(unsigned n=0;n<64;++n)
        if(s->saved_allocation[n]!=0x6d||s->saved_allocation[2U*CH_SLOT_BYTES+64U+n]!=0x6d)return false;
    return stream_rrc_guards(v);
}
static void stream_partial_pause(void *opaque)
{
    stream_state *v=opaque;vTaskDelay(1);++v->owner->stream_report->partial_yields;
}
static bool stream_partial_preflight(stream_state *v)
{
    rx_stream_report *s=v->owner->stream_report;int64_t start=esp_timer_get_time();
    s->partial_core=xPortGetCoreID();memset(v->rrc,0x6d,sizeof *v->rrc);
    bool ok=rrc_partial_controls(&v->rrc->filter,s->saved,v->rrc_output,
                                stream_partial_push,stream_partial_guards,stream_partial_pause,v,&s->partial);
    memset(s->saved_allocation,0x6d,2U*CH_SLOT_BYTES+128U);
    memset(v->rrc_memory,0x6d,RRC_MEMORY_BYTES);
    s->partial.reset=rrc4_chunk_batch_init(v->rrc,0)&&stream_rrc_guards(v);
    s->partial_stack_free=uxTaskGetStackHighWaterMark(NULL);
    s->partial_elapsed_us=(uint64_t)(esp_timer_get_time()-start);
    s->partial_passed=ok&&s->partial.reset&&s->partial_core==0&&s->partial_stack_free>=512U;
    return s->partial_passed;
}
/* Brackets actual vTaskDelay calls; all other elapsed time includes ISR and
 * preemption. No vector or borrowed pointer is used by these clock helpers. */
static void stream_sink_pause(rx_stream_report *s,int64_t *work_begin)
{
    int64_t before=esp_timer_get_time();
    vTaskDelay(1);
    int64_t after=esp_timer_get_time();
    uint64_t work=(uint64_t)(before-*work_begin),wait=(uint64_t)(after-before);
    if(work>s->max_sink_work_segment_us)s->max_sink_work_segment_us=work;
    if(wait>s->max_sink_wait_us)s->max_sink_wait_us=wait;
    s->sink_wait_elapsed_us+=wait;++s->consumer_yields;*work_begin=after;
}
static void stream_sink_done(rx_stream_report *s,int64_t start,int64_t work_begin)
{
    int64_t end=esp_timer_get_time();uint64_t work=(uint64_t)(end-work_begin);
    if(work>s->max_sink_work_segment_us)s->max_sink_work_segment_us=work;
    s->sink_elapsed_us=(uint64_t)(end-start);
    s->sink_work_elapsed_us=s->sink_elapsed_us-s->sink_wait_elapsed_us;
    s->sink_timing_measured=true;
}
static void stream_reader(void *opaque)
{
    stream_state *v=opaque;rx_stream_report *s=v->owner->stream_report;
    bool ready=stream_partial_preflight(v);
    if(!ready){++s->consumer_errors;ch_reader_abort(&v->queue);}
    /* Startup consumes one credit; a failed reader also supplies join credit. */
    xTaskNotifyGive(v->producer);
    if(!ready) {
        s->consumer_stack_free=uxTaskGetStackHighWaterMark(NULL);
        xTaskNotifyGive(v->producer);vTaskDelete(NULL);return;
    }
    int64_t last=esp_timer_get_time(),yield=last,start=last,work_begin=last;
    s->consumer_core=xPortGetCoreID();
    for(;;) {
        ch_lease x;enum ch_read got=ch_acquire(&v->queue,&x);
        if(got==CH_EMPTY){stream_sink_pause(s,&work_begin);continue;}
        if(got==CH_END){if(!ch_reader_finish(&v->queue))++s->consumer_errors;break;}
        if(got!=CH_DATA){++s->consumer_errors;ch_reader_abort(&v->queue);break;}
        int64_t before=esp_timer_get_time();
        uint32_t gap=s->consumed_chunks?(uint32_t)(before-last):0;if(gap>s->max_sink_gap_us)s->max_sink_gap_us=gap;
        if(x.meta.input_first!=s->consumed_inputs||x.meta.output_first!=s->consumed_outputs||
           !ch_guards(&v->queue,&x)) {
            ++s->consumer_errors;ch_reader_abort(&v->queue);break;
        }
        size_t bytes=2U*x.meta.output_pairs;
        /* Only bounded matched S16 output is checksummed. Whole SC8 input
         * CRC is not computed; retained first/last chunks stay independent. */
        if(!rrc4_chunk_batch_begin(v->rrc,&x)) {
            ++s->consumer_errors;ch_reader_abort(&v->queue);break;
        }
        bool matched_ok=true;
        while(v->rrc->active) {
            rrc4_chunk_batch_result part;
            if(!stream_rrc_step(v,&part)||part.matched_first!=s->matched_pairs||
               (part.output_pairs&&part.filtered_window_first!=4U*s->matched_pairs)) {
                matched_ok=false;break;
            }
            ++s->rrc_steps;
            if(part.output_pairs) {
                s->consumed_crc=esp_rom_crc32_le(s->consumed_crc,(uint8_t *)v->rrc_output,4U*part.output_pairs);
                unsigned keep=RRC_PREFIX_PAIRS-s->matched_prefix_pairs;
                if(keep>part.output_pairs)keep=part.output_pairs;
                if(keep)memcpy(s->matched_prefix+4U*s->matched_prefix_pairs,v->rrc_output,4U*keep);
                s->matched_prefix_pairs+=keep;s->matched_pairs+=part.output_pairs;
            }
        }
        s->rrc_guards=s->rrc_guards&&stream_rrc_guards(v);
        if(!matched_ok||!s->rrc_guards||!ch_guards(&v->queue,&x)) {
            ++s->consumer_errors;ch_reader_abort(&v->queue);break;
        }
        if(s->consumed_chunks==0||s->consumed_chunks==STREAM_UNITS-1U) {
            unsigned slot=s->consumed_chunks?1U:0U;
            uint8_t *saved=s->saved+slot*CH_SLOT_BYTES;
            memset(saved+bytes,0x6d,16U);memcpy(saved,x.data,bytes);
            s->sample[slot]=x.meta;s->sample_bytes[slot]=bytes;
            s->sample_crc[slot]=esp_rom_crc32_le(0,saved,bytes);
        }
        s->consumed_inputs+=x.meta.input_pairs;s->consumed_outputs+=x.meta.output_pairs;
        ++s->consumed_chunks;
        if(!ch_release(&v->queue,&x)) {
            ++s->consumer_errors;ch_reader_abort(&v->queue);break;
        }
        last=esp_timer_get_time();uint32_t cost=(uint32_t)(last-before);
        if(cost>s->max_sink_us)s->max_sink_us=cost;
        if(last-yield>=STREAM_SINK_YIELD_US){stream_sink_pause(s,&work_begin);yield=esp_timer_get_time();}
    }
    s->matched_prefix_guards=matched_prefix_guards(s);
    s->matched_prefix_crc=esp_rom_crc32_le(0,s->matched_prefix,4U*s->matched_prefix_pairs);
    s->consumer_stack_free=uxTaskGetStackHighWaterMark(NULL);
    if(!s->matched_prefix_guards||s->consumer_stack_free<512U) {
        ++s->consumer_errors;ch_reader_abort(&v->queue);
    }
    stream_sink_done(s,start,work_begin);
    /* All borrowed IQ pointers and PIE values are dead before notification. */
    xTaskNotifyGive(v->producer);vTaskDelete(NULL);
}
static bool IRAM_ATTR stream_filter(void *opaque,unsigned stage,unsigned count,uint8_t *out,unsigned expected)
{
    (void)stage;run *r=opaque;rx_report *s=r->report;stream_state *v=r->stream;
    if(!s->retained_pairs&&count>=CONTROL_PAIRS) {
        rx_copy(bank(v->bank),v->first,CONTROL_PAIRS,r->snapshot);s->snapshot_pairs=CONTROL_PAIRS;
    }
    hb_vector_scratch *scratch=(hb_vector_scratch *)(r->stage+2U*FILTER_STAGE+64U);
    size_t produced=0;uint32_t before=esp_cpu_get_cycle_count();
    bool ok=hb_vector_bank(r->filter_state,scratch,HB_TILE,bank(v->bank),v->first,count,
                           (int8_t *)out,HB_MAX_OUTPUT,&produced);
    uint32_t elapsed=esp_cpu_get_cycle_count()-before;
    if(elapsed>s->max_pack_cycles)s->max_pack_cycles=elapsed;
    if(!ok){++s->sentinel_errors;return false;}
    if(!filter_guards(r)){s->stage_guards_ok=false;return false;}
    return produced==expected;
}
static bool IRAM_ATTR stream_dma_finish(void *opaque)
{
    run *r=opaque;rx_report *s=r->report;esp_err_t e=rx_spill_finish(r->spill);
    s->dma_rx_flags|=r->spill->rx_flags;s->dma_tx_flags|=r->spill->tx_flags;
    if(e!=ESP_OK){++s->dma_errors;return false;}
    if(!filter_guards(r)){s->stage_guards_ok=false;return false;}
    s->max_transfer_cycles=r->spill->max_transfer_cycles;
    s->max_wait_cycles=r->spill->max_wait_cycles;
    s->max_blocked_wait_cycles=r->spill->max_blocked_wait_cycles;
    s->max_completion_cycles=r->spill->max_completion_cycles;
    s->max_submit_cycles=r->spill->max_submit_cycles;
    s->blocked_waits=r->spill->blocked_waits;
    s->notification_errors=r->spill->notification_errors;
    s->total_wait_cycles=r->spill->total_wait_cycles;
    s->total_blocked_wait_cycles=r->spill->total_blocked_wait_cycles;
    s->total_completion_cycles=r->spill->total_completion_cycles;
    ++s->dma_copies;return true;
}
static bool IRAM_ATTR stream_dma_begin(void *opaque,const uint8_t *src,unsigned bytes,uint8_t *dst)
{
    run *r=opaque;esp_err_t e=rx_spill_packed_begin(r->spill,src,bytes,dst);
    if(e!=ESP_OK)++r->report->dma_errors;
    return e==ESP_OK;
}
static bool IRAM_ATTR stream_clear(void *opaque)
{run *r=opaque;clear_bank(r->stream->bank);return true;}
static void IRAM_ATTR stream_stop(void *opaque)
{
    run *r=opaque;REG_WRITE(DUMP_CTRL,0);REG_WRITE(BANK_USAGE,r->stream->ownership);
    r->stream_report->idle_visits=ch_idle_count()-r->stream->idle_start;
    __asm__ volatile("memw":::"memory");
}
static bool IRAM_ATTR stream_dma_abort(void *opaque)
{run *r=opaque;return rx_spill_abort(r->spill)==ESP_OK;}
static uint32_t IRAM_ATTR stream_cycles(void *opaque)
{(void)opaque;return esp_cpu_get_cycle_count();}

static esp_err_t stream_open(run *r)
{
    stream_state *v=heap_caps_aligned_alloc(64,sizeof *v,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if(!v)return ESP_ERR_NO_MEM;
    memset(v,0,sizeof *v);
    v->owner=r;v->producer=xTaskGetCurrentTaskHandle();
    v->ownership=REG_READ(BANK_USAGE)&~15U;
    v->arena=heap_caps_aligned_alloc(64,CH_MAX_SLOTS*CH_SLOT_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    rx_stream_report *s=r->stream_report;
    s->saved_allocation=heap_caps_aligned_alloc(64,2U*CH_SLOT_BYTES+128U,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    s->rrc_free_internal_before=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    s->rrc_largest_internal_before=heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    v->rrc_memory=heap_caps_aligned_alloc(64,RRC_MEMORY_BYTES,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    s->matched_allocation=heap_caps_aligned_alloc(64,RRC_OUTPUT_BYTES+128U,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!v->arena||!s->saved_allocation||!v->rrc_memory||!s->matched_allocation) {
        free(v->arena);free(s->saved_allocation);free(v->rrc_memory);free(s->matched_allocation);
        s->saved_allocation=s->matched_allocation=NULL;free(v);return ESP_ERR_NO_MEM;
    }
    memset(s->saved_allocation,0x6d,2U*CH_SLOT_BYTES+128U);s->saved=s->saved_allocation+64U;
    memset(v->rrc_memory,0x6d,RRC_MEMORY_BYTES);
    v->rrc=(void *)(v->rrc_memory+64U);v->rrc_output=(void *)(v->rrc_memory+128U+sizeof *v->rrc);
    memset(s->matched_allocation,0x6d,RRC_OUTPUT_BYTES+128U);s->matched_prefix=s->matched_allocation+64U;
    s->rrc_allocated=rrc4_chunk_batch_init(v->rrc,0);s->rrc_context_ok=s->rrc_guards=s->rrc_allocated;
    if(!s->rrc_allocated) {
        free(v->arena);free(s->saved_allocation);free(v->rrc_memory);free(s->matched_allocation);
        s->saved_allocation=s->matched_allocation=s->saved=s->matched_prefix=NULL;free(v);return ESP_FAIL;
    }
    ch_ops ops={stream_filter,stream_dma_finish,stream_dma_begin,stream_clear,stream_stop,stream_dma_abort,stream_cycles};
    if(!ch_init(&v->queue,v->arena,CH_MAX_SLOTS*CH_SLOT_BYTES,CH_MAX_SLOTS,0)||
       !ch_pipe_init(&v->pipe,&v->queue,r->stage,&ops,r)) {
        free(v->arena);free(s->saved_allocation);free(v->rrc_memory);free(s->matched_allocation);
        s->saved_allocation=s->saved=s->matched_allocation=s->matched_prefix=NULL;free(v);return ESP_FAIL;
    }
    r->stream=v;
    if(!ch_idle_open())return ESP_ERR_INVALID_STATE;
    v->idle_registered=true;
    (void)ulTaskNotifyTake(pdTRUE,0);
    if(xTaskCreatePinnedToCore(stream_reader,"chunk_sink",8192,v,5,NULL,0)!=pdPASS) {
        ch_reader_abort(&v->queue);return ESP_ERR_NO_MEM;
    }
    v->reader_started=true;
    (void)ulTaskNotifyTake(pdFALSE,portMAX_DELAY);
    return s->partial_passed?ESP_OK:ESP_FAIL;
}
static esp_err_t IRAM_ATTR stream_unit(run *r,unsigned current,uint16_t first,size_t count)
{
    stream_state *v=r->stream;rx_report *s=r->report;v->bank=current;v->first=first;
    uint32_t before=esp_cpu_get_cycle_count();
    bool ok=ch_pipe_step(&v->pipe,count);
    s->output_pairs=v->queue.output_pairs;
    s->pending_output_bytes=v->pipe.dma_pending?2U*v->pipe.pending.meta.output_pairs:0;
    uint32_t published=atomic_load_explicit(&v->queue.published,memory_order_relaxed);
    if(published){ch_meta *m=&v->queue.meta[(published-1U)&(CH_MAX_SLOTS-1U)];s->committed_output_pairs=m->output_first+m->output_pairs;}
    uint32_t released=atomic_load_explicit(&v->queue.released,memory_order_acquire);
    unsigned queued=v->queue.reserved-released;
    if(queued>r->stream_report->max_queued_chunks)r->stream_report->max_queued_chunks=queued;
    if(queued>r->stream_report->interval_max_queued)r->stream_report->interval_max_queued=queued;
    uint32_t elapsed=esp_cpu_get_cycle_count()-before;
    if(elapsed>s->max_copy_cycles)s->max_copy_cycles=elapsed;
    if(elapsed>FILTER_BUDGET&&ok){(void)ch_pipe_stop(&v->pipe,false);ok=false;}
    if(!ok){if(queued>=CH_MAX_SLOTS)++s->overruns;else ++s->filter_deadline_errors;return ESP_FAIL;}
    return ESP_OK;
}
static void stream_before_close(run *r)
{
    if(!r->stream)return;
    if(!r->stream->pipe.stopped)(void)ch_pipe_stop(&r->stream->pipe,false);
    if(r->stream->idle_registered){ch_idle_close();r->stream->idle_registered=false;}
}
static void stream_join(run *r)
{
    stream_state *v=r->stream;if(!v)return;rx_stream_report *s=r->stream_report;
    /* rx_spill_close has returned: its stop/idle proof precedes handle deletion.
     * This also resolves a prior transient abort failure before any free. */
    if(!v->pipe.dma_idle)ch_end_stream(&v->queue,false);
    if(v->reader_started){(void)ulTaskNotifyTake(pdFALSE,portMAX_DELAY);s->consumer_joined=true;}
    else {ch_reader_abort(&v->queue);s->consumer_joined=true;}
    s->saved_guards=true;
    for(unsigned n=0;n<64;++n)if(s->saved_allocation[n]!=0x6d||s->saved_allocation[2U*CH_SLOT_BYTES+64U+n]!=0x6d)s->saved_guards=false;
    for(unsigned slot=0;slot<2;++slot)for(unsigned n=0;n<16;++n)
        if(s->saved[slot*CH_SLOT_BYTES+s->sample_bytes[slot]+n]!=0x6d)s->saved_guards=false;
    if(!ch_reclaimable(&v->queue))abort(); /* cannot happen after proved idle+joined reader */
    s->complete=ch_succeeded(&v->queue)&&s->consumer_errors==0&&s->consumer_joined&&
        s->partial_passed&&
        s->consumed_chunks==STREAM_UNITS&&s->consumed_inputs==r->report->retained_pairs&&
        s->consumed_outputs==r->report->committed_output_pairs&&s->saved_guards&&
        s->matched_pairs==(s->consumed_outputs>=65U?1U+(s->consumed_outputs-65U)/4U:0U)&&
        s->rrc_allocated&&s->rrc_context_ok&&s->rrc_guards&&stream_rrc_guards(v)&&
        s->matched_prefix_pairs==RRC_PREFIX_PAIRS&&s->matched_prefix_guards;
    free(v->rrc_memory);s->rrc_freed=true;
    s->rrc_free_internal_after=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    free(v->arena);s->arena_freed=true;free(v);r->stream=NULL;s->queue_freed=true;
}

static esp_err_t stage_open(run *r)
{
    rx_report *s = r->report;
    const unsigned caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
    s->stage_bytes = FILTER_WORKSPACE;
    s->stage_free_internal = heap_caps_get_free_size(caps);
    s->stage_largest_internal = heap_caps_get_largest_free_block(caps);
    r->stage = heap_caps_aligned_alloc(64, FILTER_WORKSPACE, caps);
    s->stage_allocated = r->stage != NULL;
    s->stage_guards_ok = s->stage_allocated;
    if (!r->stage) return ESP_ERR_NO_MEM;
    memset(r->stage, 0x6d, FILTER_WORKSPACE);
    return ESP_OK;
}
typedef struct {
    uint64_t pairs;
    uint32_t cycle, changed;
    uint16_t index;
} position;

static bool IRAM_ATTR advance(position *p, rx_report *s)
{
    uint32_t sampled = esp_cpu_get_cycle_count();
    uint16_t index = REG_READ(DUMP_INDEX) & RX_BANK_MASK;
    uint32_t now = esp_cpu_get_cycle_count();
    uint32_t elapsed = now - p->cycle;
    if (elapsed > s->max_poll_cycles) s->max_poll_cycles = elapsed;
    /* A full unobserved revolution would make the modulo index ambiguous. */
    if (elapsed >= (RX_BANK_PAIRS - RX_EDGE_GUARD) * PAIR_CYCLES) {
        ++s->overruns;
        return false;
    }
    unsigned delta = (index - p->index) & RX_BANK_MASK;
    p->pairs += delta;
    p->index = index;
    p->cycle = sampled;
    if (delta) p->changed = now;
    if (now - p->changed >= RX_BANK_PAIRS * PAIR_CYCLES) {
        ++s->overruns; /* The engine stopped or its index is no longer advancing. */
        return false;
    }
    return true;
}
static void IRAM_ATTR unit_boundary(run *r,uint32_t saved_irq,uint32_t service_start)
{
    rx_report *s=r->report;
    {
        uint32_t before=esp_cpu_get_cycle_count(),resumed;
        __asm__ volatile(".rept 32\n nop\n .endr":::"memory");
        taskYIELD();
        __asm__ volatile("rsr %0,ps":"=a"(resumed));
        uint32_t elapsed=esp_cpu_get_cycle_count()-before;
        ++s->irq_windows;
        if(elapsed>s->max_irq_window_cycles)s->max_irq_window_cycles=elapsed;
        if(elapsed>23885U)++s->irq_window_budget_errors;
        if((resumed&15U)!=(saved_irq&15U))++s->irq_context_errors;
    }
    uint32_t elapsed=esp_cpu_get_cycle_count()-service_start;
    if(elapsed>s->max_unit_with_window_cycles)s->max_unit_with_window_cycles=elapsed;
    if(elapsed>FILTER_BUDGET)++s->irq_unit_budget_errors;
}
static esp_err_t IRAM_ATTR __attribute__((noinline)) collect(run *r)
{
    rx_report *s = r->report;
    const uint32_t ownership = REG_READ(BANK_USAGE) & ~15U;
    r->stream->ownership = ownership;
    REG_WRITE(DUMP_CTRL, 0);
    REG_WRITE(DAC_CTRL, 0);
    REG_WRITE(BANK_USAGE, ownership);
    __asm__ volatile ("memw" ::: "memory");
    for (unsigned n = 0; n < 3; ++n) clear_bank(n);
    REG_WRITE(DUMP_LANES, 0x000c2040U);
    REG_WRITE(BANK_USAGE, ownership | 1U);

    /* Startup remains a short protected bracket. Streaming immediately restores
     * ordinary IRQ service; bounded leaves protect their own complete context. */
    uint32_t irq = portSET_INTERRUPT_MASK_FROM_ISR();
    uint32_t epoch = esp_cpu_get_cycle_count();
    const uint64_t engine_limit = STREAM_LIMIT_CYCLES;
    s->irq_saved_level = irq & 15U;
    s->ticks_at_start = core1_ticks;
    {
        r->stream->epoch = epoch; r->stream->idle_start = ch_idle_count();
        stream_epoch_init(&r->stream->clock, epoch, s->ticks_at_start);
        r->stream_report->epoch_valid = true;
    }
    REG_WRITE(DUMP_CTRL, (1U << 31) | (1U << 17) | (1U << 16) | RX_BANK_PAIRS);

    portCLEAR_INTERRUPT_MASK_FROM_ISR(irq);
    position p = {.cycle = epoch, .changed = epoch};
    rx_edge begin = {0, 0};
    uint64_t target = UNIT_PAIRS;
    uint16_t expected = 0;
    unsigned current = 0;
    esp_err_t error = ESP_FAIL;
    bool boundary_pending = false;
    uint32_t service_start = 0;
    while (s->units < STREAM_UNITS) {
        if (boundary_pending) {
            boundary_pending = false;
            unit_boundary(r, irq, service_start);
        }
        uint32_t deadline_now = esp_cpu_get_cycle_count();
        uint64_t elapsed = (uint32_t)(deadline_now - epoch);
        if (!stream_epoch_peek(&r->stream->clock, deadline_now, &elapsed)) {
            error = ESP_ERR_INVALID_STATE;
            break;
        }
        if (elapsed >= engine_limit) {
            ++s->engine_timeouts;
            error = ESP_ERR_TIMEOUT;
            break;
        }
        if (!advance(&p, s)) break;
        if (s->irq_window_budget_errors || s->irq_unit_budget_errors ||
            s->irq_context_errors) break;
        if (p.pairs < target) continue;
        uint32_t late = p.pairs - target;
        if (late > s->max_switch_late_pairs) s->max_switch_late_pairs = late;
        if (late >= LATE_PAIRS) { ++s->late; break; }
        unsigned next = (current + 1U) % 3U;
        /* Unrelated accounting can exceed the edge limit. Read immediately
         * around the ownership write instead; keep the same64-pair bound. */
        uint32_t handoff_irq=portSET_INTERRUPT_MASK_FROM_ISR();
        rx_edge end = {.before = REG_READ(DUMP_INDEX) & RX_BANK_MASK};
        REG_WRITE(BANK_USAGE, ownership | (1U << next));
        __asm__ volatile ("memw" ::: "memory");
        end.after = REG_READ(DUMP_INDEX) & RX_BANK_MASK;
        portCLEAR_INTERRUPT_MASK_FROM_ISR(handoff_irq);
        uint32_t switch_now = esp_cpu_get_cycle_count();
        if (!stream_time_add(r, switch_now, core1_ticks)) {
            error = ESP_ERR_INVALID_STATE;
            break;
        }
        late += (end.after - p.index) & RX_BANK_MASK;
        if (late > s->max_switch_late_pairs) s->max_switch_late_pairs = late;
        if (late >= LATE_PAIRS) { ++s->late; break; }
        if (!advance(&p, s)) break;

        uint16_t first;
        size_t count;
        const uint32_t *src = bank(current);
        if (!rx_span(src, begin, end, &first, &count)) { ++s->boundary_errors; break; }
        if (count < UNIT_PAIRS - LATE_PAIRS || count > UNIT_PAIRS + LATE_PAIRS) {
            ++s->boundary_errors;
            break;
        }
        if (first != expected) { ++s->gaps; break; }
        expected = (first + count) & RX_BANK_MASK;
        s->acquired_pairs += count;
        ++s->units;
        size_t keep = count;

        service_start = esp_cpu_get_cycle_count();
        if (stream_unit(r, current, first, keep) != ESP_OK) break;
        s->retained_pairs += keep;
        if (s->units % STREAM_CHECKPOINT_UNITS == 0) {
            stream_checkpoint_save(r, true);
            if (!r->stream_report->epoch_valid) { error = ESP_ERR_INVALID_STATE; break; }
        }
        if (s->units == STREAM_UNITS) { error = ESP_OK; break; }

        begin = end;
        current = next;
        target += UNIT_PAIRS;
        boundary_pending = true;
    }
    REG_WRITE(DUMP_CTRL, 0);
    s->ticks_at_engine_stop = core1_ticks;
    {
        uint32_t stop_now = esp_cpu_get_cycle_count();
        if (!stream_time_add(r, stop_now, s->ticks_at_engine_stop)) error = ESP_ERR_INVALID_STATE;
        s->engine_cycles = r->stream_report->epoch_valid ? r->stream->clock.total : 0;
        stream_checkpoint_save(r, false);
    }
    REG_WRITE(BANK_USAGE, ownership);
    if (s->engine_cycles >= engine_limit && !s->engine_timeouts) {
        ++s->engine_timeouts;
        error = ESP_ERR_TIMEOUT;
    }
    uint32_t drain_start = esp_cpu_get_cycle_count();
    esp_err_t drained = ESP_OK;
    {
        if (!ch_pipe_stop(&r->stream->pipe, error == ESP_OK)) drained = ESP_FAIL;
        uint32_t published = atomic_load_explicit(&r->stream->queue.published, memory_order_acquire);
        if (published) {
            ch_meta *m = &r->stream->queue.meta[(published-1U) & (CH_MAX_SLOTS-1U)];
            s->committed_output_pairs = m->output_first + m->output_pairs;
        }
        s->pending_output_bytes = 0;
    }
    s->final_drain_cycles = esp_cpu_get_cycle_count() - drain_start;
    s->filter_drained = drained == ESP_OK && !s->pending_output_bytes;
    if (drained != ESP_OK) error = drained;
    {
        uint32_t drained_now = esp_cpu_get_cycle_count();
        uint32_t drained_ticks = core1_ticks;
        if (!stream_time_add(r, drained_now, drained_ticks)) error = ESP_ERR_INVALID_STATE;
        s->elapsed_cycles = r->stream_report->epoch_valid ? r->stream->clock.total : 0;
    }
    s->ticks_before_unmask = core1_ticks;
    s->max_protected_leaf_cycles=hb_leaf_max_cycles;
    s->protected_leaf_calls=hb_leaf_calls;
    uint32_t restored_ps;
    __asm__ volatile ("rsr %0, ps" : "=a"(restored_ps));
    s->irq_restored_level = restored_ps & 15U;
    return error;
}
static void worker(void *arg)
{
    run *r = arg;
    rx_report *s = r->report;
    bool initialized = false, started = false, rf_setup = false, lo_saved = false;
    bool tick_hook = false;
    s->worker_core = xPortGetCoreID();
    rx_spill spill = {0};
    hb_tile_state filter_state = {0};
    r->filter_state = &filter_state;
    r->spill = &spill;
    s->error = rx_spill_open(&spill);
    if (s->error != ESP_OK) goto done;
    if (!rx_plan_lo(r->mhz, &s->tuning)) { s->error = ESP_ERR_INVALID_ARG; goto done; }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = 0;
    s->error = esp_wifi_init(&cfg);
    if (s->error != ESP_OK) goto done;
    initialized = true;
    if ((s->error = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK ||
        (s->error = esp_wifi_set_mode(WIFI_MODE_NULL)) != ESP_OK ||
        (s->error = esp_wifi_start()) != ESP_OK) goto done;
    started = true;
    if ((s->error = esp_wifi_set_ps(WIFI_PS_NONE)) != ESP_OK ||
        (s->error = esp_wifi_set_promiscuous(true)) != ESP_OK ||
        (s->error = esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE)) != ESP_OK) goto done;

    s->error = stage_open(r); /* Wi-Fi is live when allocation is measured. */
    if (s->error != ESP_OK) goto done;
    s->error = stream_open(r);
    if (s->error != ESP_OK) goto done;
    rf_setup = true;
    REG_WRITE(DUMP_CTRL, 0);
    REG_WRITE(DAC_CTRL, 0);
    force_txon_mode(0, 0, 0);
    REG_CLR_BIT(FE_CTRL, 1U << 1);
    stop_tx_tone(1);
    rom_pbus_workmode();
    rom_pbus_xpd_tx_off();
    s->lo_saved = regi2c_ctrl_read_reg(0x65, 0, 0);
    lo_saved = true;
    if (!lo_select(false, &s->lo_active)) { s->error = ESP_ERR_INVALID_STATE; goto done; }
    set_chanfreq(2412, 0);
    phy_set_freq(s->tuning.pll_khz / 1000U, s->tuning.pll_khz % 1000U);
    rom_pbus_xpd_rx_on(1);
    rom_set_rxclk_en(1);
    s->gain_max = (REG_READ(AGC_FORCE) >> 8) & 0x7f;
    if (r->gain > s->gain_max) { s->error = ESP_ERR_INVALID_ARG; goto done; }
    force_rx_gain(1, r->gain, 0);
    s->lo_applied = lo_select(s->tuning.divided, &s->lo_active);
    if (!s->lo_applied) { s->error = ESP_ERR_INVALID_STATE; goto done; }
    vTaskDelay(pdMS_TO_TICKS(3));
    {
        uint32_t owner=REG_READ(BANK_USAGE);
        if((REG_READ(DUMP_CTRL)&(1U<<31))||(REG_READ(DAC_CTRL)&(1U<<31))) {
            s->error=ESP_ERR_INVALID_STATE;goto done;
        }
        REG_WRITE(BANK_USAGE,owner&~15U);__asm__ volatile("memw":::"memory");
        bool context_ok=hb_context_controls(bank(0),(hb_vector_scratch *)(r->stage+2U*FILTER_STAGE+64U),
                                            &s->leaf_context);
        REG_WRITE(BANK_USAGE,owner);__asm__ volatile("memw":::"memory");
        memset(r->stage,0x6d,FILTER_WORKSPACE);memset(r->filter_state,0,sizeof *r->filter_state);
        hb_leaf_max_cycles=hb_leaf_calls=0;
        if(!context_ok){s->error=ESP_FAIL;goto done;}
        s->error=rx_spill_notifications(r->spill);
        if(s->error!=ESP_OK)goto done;
    }
    {
        s->error = esp_register_freertos_tick_hook_for_cpu(count_core1_tick, 1);
        if (s->error != ESP_OK) goto done;
        tick_hook = s->tick_hook_registered = true;
        /* All RX allocations and both workers are live; ADC has not started. */
        const unsigned internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        const unsigned psram_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        s->ready_free_internal = heap_caps_get_free_size(internal_caps);
        s->ready_largest_internal = heap_caps_get_largest_free_block(internal_caps);
        s->ready_free_psram = heap_caps_get_free_size(psram_caps);
        s->ready_largest_psram = heap_caps_get_largest_free_block(psram_caps);
        s->ready_memory_measured = true;
        s->error = collect(r);
    }

done:
    stream_before_close(r);
    if (tick_hook) {
        esp_deregister_freertos_tick_hook_for_cpu(count_core1_tick, 1);
        s->tick_hook_deregistered = true;
    }
    if (r->spill) { s->cleanup_error = rx_spill_close(r->spill); r->spill = NULL; }
    if (rf_setup) {
        REG_WRITE(DUMP_CTRL, 0);
        REG_WRITE(DAC_CTRL, 0);
        REG_CLR_BIT(FE_CTRL, 1U << 1);
        REG_WRITE(BANK_USAGE, REG_READ(BANK_USAGE) & ~15U);
        rom_pbus_xpd_tx_off();
        rom_pbus_xpd_rx_on(0);
        rom_set_rxclk_en(0);
    }
    if (lo_saved) {
        s->lo_restore_ok = lo_select((s->lo_saved & 0x10U) != 0, &s->lo_restored);
        if (!s->lo_restore_ok && s->cleanup_error == ESP_OK)
            s->cleanup_error = ESP_ERR_INVALID_STATE;
    }
    if (started) {
        esp_err_t stopped = esp_wifi_set_promiscuous(false);
        if (s->cleanup_error == ESP_OK) s->cleanup_error = stopped;
    }
    if (initialized) {
        esp_err_t stopped = esp_wifi_stop();
        if (stopped != ESP_OK && stopped != ESP_ERR_WIFI_NOT_STARTED &&
            s->cleanup_error == ESP_OK) s->cleanup_error = stopped;
        esp_err_t deinitialized = esp_wifi_deinit();
        if (deinitialized != ESP_OK && s->cleanup_error == ESP_OK)
            s->cleanup_error = deinitialized;
    }
    if (s->error == ESP_OK) s->error = s->cleanup_error;
    {
        stream_join(r);
        if (s->error == ESP_OK && !r->stream_report->complete) s->error = ESP_FAIL;
    }
    if (r->stage) {
        if (!filter_guards(r)) s->stage_guards_ok = false;
        s->output_guards_ok = r->stream_report->saved_guards;
        free(r->stage); /* DMA close/idle and RF cleanup have already completed. */
        r->stage = NULL;
        s->stage_freed = true;
        const unsigned caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
        s->stage_free_after = heap_caps_get_free_size(caps);
        s->stage_largest_after = heap_caps_get_largest_free_block(caps);
        if (!s->stage_guards_ok || !s->output_guards_ok) s->error = ESP_FAIL;
    }
    if (s->snapshot_pairs)
        s->snapshot_crc32 = esp_rom_crc32_le(0, (uint8_t *)r->snapshot, 4U * s->snapshot_pairs);
    s->worker_stack_free=uxTaskGetStackHighWaterMark(NULL);
    xSemaphoreGive(r->done);
    vTaskDelete(NULL);
}
typedef struct {rx_report *report;SemaphoreHandle_t done;} leaf_run;
static void leaf_worker(void *opaque)
{
    leaf_run *job=opaque;rx_report *s=job->report;wifi_mode_t mode;
    s->worker_core=xPortGetCoreID();s->error=ESP_ERR_INVALID_STATE;
    if(s->worker_core!=1||esp_wifi_get_mode(&mode)!=ESP_ERR_WIFI_NOT_INIT||
       REG_READ(DUMP_CTRL)||REG_READ(DAC_CTRL)||(REG_READ(BANK_USAGE)&15U))goto finished;
    const unsigned caps=MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA|MALLOC_CAP_8BIT;
    s->stage_bytes=FILTER_WORKSPACE;s->stage_free_internal=heap_caps_get_free_size(caps);
    s->stage_largest_internal=heap_caps_get_largest_free_block(caps);
    uint8_t *workspace=heap_caps_aligned_alloc(64,FILTER_WORKSPACE,caps);
    if(!workspace){s->error=ESP_ERR_NO_MEM;goto finished;}
    s->stage_allocated=true;memset(workspace,0x6d,FILTER_WORKSPACE);
    hb_leaf_max_cycles=hb_leaf_calls=0;
    bool ok=hb_context_controls(bank(0),(hb_vector_scratch *)(workspace+2U*FILTER_STAGE+64U),&s->leaf_context);
    s->stage_guards_ok=true;
    for(unsigned n=0;n<64;++n)
        if(workspace[2U*FILTER_STAGE+n]!=0x6d||workspace[FILTER_WORKSPACE-64U+n]!=0x6d)s->stage_guards_ok=false;
    s->max_protected_leaf_cycles=hb_leaf_max_cycles;s->protected_leaf_calls=hb_leaf_calls;
    free(workspace);s->stage_freed=true;
    s->error=ok&&s->stage_guards_ok?ESP_OK:ESP_FAIL;
finished:
    s->worker_stack_free=uxTaskGetStackHighWaterMark(NULL);
    if(s->worker_stack_free<512U)s->error=ESP_FAIL;
    xSemaphoreGive(job->done);vTaskDelete(NULL);
}
esp_err_t rx_leafcheck(rx_report *report)
{
    memset(report,0,sizeof *report);StaticSemaphore_t storage;
    leaf_run job={.report=report,.done=xSemaphoreCreateBinaryStatic(&storage)};
    if(xTaskCreatePinnedToCore(leaf_worker,"leaf-abi",8192,&job,5,NULL,1)!=pdPASS)
        return report->error=ESP_ERR_NO_MEM;
    xSemaphoreTake(job.done,portMAX_DELAY);return report->error;
}
static esp_err_t execute(run *r)
{
    StaticSemaphore_t storage;
    r->done = xSemaphoreCreateBinaryStatic(&storage);
    if (xTaskCreatePinnedToCore(worker, "capture", 8192, r, tskIDLE_PRIORITY,
                                NULL, 1) != pdPASS)
        return r->report->error = ESP_ERR_NO_MEM;
    xSemaphoreTake(r->done, portMAX_DELAY);
    return r->report->error;
}
esp_err_t rx_stream(uint16_t mhz,uint8_t gain,uint32_t snapshot[512],rx_stream_report *out)
{
    if(!out||!snapshot)return ESP_ERR_INVALID_ARG;
    memset(out,0,sizeof *out);
    if(mhz<RX_FREQ_MIN_MHZ||mhz>RX_FREQ_MAX_MHZ)return out->capture.error=ESP_ERR_INVALID_ARG;
    if(esp_clk_cpu_freq()!=240000000)return out->capture.error=ESP_ERR_INVALID_STATE;
    run r={.snapshot=snapshot,.report=&out->capture,.mhz=mhz,.gain=gain,
           .stream_report=out};
    return execute(&r);
}
void rx_stream_release(rx_stream_report *s)
{
    if(s){free(s->saved_allocation);free(s->matched_allocation);s->saved_allocation=s->saved=s->matched_allocation=s->matched_prefix=NULL;s->complete=false;}
}
