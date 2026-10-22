#include "rx_queue.h"
#include <string.h>

_Static_assert(sizeof(uint32_t)==4,"ticket width");
static uint8_t *CH_HOT slot(const ch_queue *q,uint32_t ticket)
{return q->arena+(ticket&(q->slots-1U))*CH_SLOT_BYTES;}
static uint8_t *CH_HOT data(const ch_queue *q,uint32_t ticket,const ch_meta *m)
{return slot(q,ticket)+64U+((2U*m->output_first)&63U);}
static bool CH_HOT same(const ch_queue *q,const ch_lease *x)
{
    const ch_meta *m=&q->meta[x->ticket&(q->slots-1U)];
    return x->data==data(q,x->ticket,m) && x->meta.input_first==m->input_first &&
        x->meta.output_first==m->output_first && x->meta.input_pairs==m->input_pairs &&
        x->meta.output_pairs==m->output_pairs;
}
bool ch_init(ch_queue *q,void *arena,size_t bytes,unsigned slots,uint32_t seed)
{
    if(!q||!arena||((uintptr_t)arena&63U)||slots<2||slots>CH_MAX_SLOTS||
       (slots&(slots-1U))||bytes!=(size_t)slots*CH_SLOT_BYTES)return false;
    memset(q,0,sizeof *q);q->arena=arena;q->slots=slots;q->reserved=q->reading=seed;
    atomic_init(&q->published,seed);atomic_init(&q->released,seed);
    atomic_init(&q->terminal,CH_RUNNING);atomic_init(&q->reader_done,false);
    atomic_init(&q->writer_done,false);atomic_init(&q->reader_failed,false);
    return true; /* Only aligned32-bit atomic loads/stores; no atomic RMW. */
}
bool CH_HOT ch_reserve(ch_queue *q,unsigned count,ch_lease *x)
{
    if(!q||!x||count<CH_UNIT-CH_LATE||count>CH_UNIT+CH_LATE||
       atomic_load_explicit(&q->terminal,memory_order_acquire)!=CH_RUNNING||
       atomic_load_explicit(&q->reader_failed,memory_order_acquire))return false;
    uint32_t released=atomic_load_explicit(&q->released,memory_order_acquire);
    uint32_t published=atomic_load_explicit(&q->published,memory_order_relaxed);
    if(q->reserved-released>=q->slots || q->reserved-published>=2U)return false;
    if(q->input_pairs>UINT64_MAX-count)return false;
    uint64_t total=q->input_pairs+count,total_out=(total-27U)/4U;
    if(total_out<q->output_pairs || total_out-q->output_pairs>UINT32_MAX)return false;
    ch_meta m={q->input_pairs,q->output_pairs,count,(uint32_t)(total_out-q->output_pairs)};
    uint8_t *out=data(q,q->reserved,&m);
    if(out+2U*m.output_pairs+16U>slot(q,q->reserved)+CH_SLOT_BYTES)return false;
    q->meta[q->reserved&(q->slots-1U)]=m;
    *x=(ch_lease){q->reserved,m,out};
    memset(out-16U,0x6d,16U);memset(out+2U*m.output_pairs,0x6d,16U);
    ++q->reserved;q->input_pairs=total;q->output_pairs=total_out;return true;
}
bool CH_HOT ch_guards(const ch_queue *q,const ch_lease *x)
{
    if(!q||!x||!same(q,x))return false;
    for(unsigned n=0;n<16;++n)
        if(x->data[(int)n-16]!=0x6d||x->data[2U*x->meta.output_pairs+n]!=0x6d)return false;
    return true;
}
bool CH_HOT ch_publish(ch_queue *q,const ch_lease *x)
{
    if(!q||!x||atomic_load_explicit(&q->terminal,memory_order_relaxed)!=CH_RUNNING||
       atomic_load_explicit(&q->reader_failed,memory_order_acquire)||
       x->ticket!=atomic_load_explicit(&q->published,memory_order_relaxed)||
       q->reserved-x->ticket<1U||q->reserved-x->ticket>2U||!ch_guards(q,x))return false;
    /* All CPU/DMA/cache completion is the caller's prior obligation. This
     * release publishes initialized metadata+PSRAM bytes, never a pending DMA. */
    atomic_store_explicit(&q->published,x->ticket+1U,memory_order_release);return true;
}
void CH_HOT ch_end_stream(ch_queue *q,bool success)
{atomic_store_explicit(&q->terminal,success&&q->reserved==atomic_load_explicit(&q->published,memory_order_acquire)&&!atomic_load_explicit(&q->reader_failed,memory_order_acquire)?CH_EOF:CH_FAILED,memory_order_release);
 atomic_store_explicit(&q->writer_done,true,memory_order_release);}
void CH_HOT ch_fail_active(ch_queue *q)
{atomic_store_explicit(&q->terminal,CH_FAILED,memory_order_release);}
enum ch_read ch_acquire(ch_queue *q,ch_lease *x)
{
    if(!q||!x||q->borrowed||atomic_load_explicit(&q->reader_done,memory_order_relaxed))return CH_ERROR;
    unsigned end=atomic_load_explicit(&q->terminal,memory_order_acquire);
    if(end==CH_FAILED||atomic_load_explicit(&q->reader_failed,memory_order_acquire))return CH_ERROR;
    uint32_t published=atomic_load_explicit(&q->published,memory_order_acquire);
    if(published==q->reading)return end==CH_EOF?CH_END:CH_EMPTY;
    if(published-q->reading>q->slots)return CH_ERROR;
    ch_meta m=q->meta[q->reading&(q->slots-1U)];
    *x=(ch_lease){q->reading,m,data(q,q->reading,&m)};q->borrowed=true;return CH_DATA;
}
bool ch_release(ch_queue *q,const ch_lease *x)
{
    if(!q||!x||!q->borrowed||x->ticket!=q->reading||!ch_guards(q,x))return false;
    q->borrowed=false;++q->reading;
    atomic_store_explicit(&q->released,q->reading,memory_order_release);return true;
}
void ch_reader_reject(ch_queue *q)
{atomic_store_explicit(&q->reader_failed,true,memory_order_release);}
void ch_reader_abort(ch_queue *q)
{
    ch_reader_reject(q);q->borrowed=false;
    q->reading=atomic_load_explicit(&q->published,memory_order_acquire);
    atomic_store_explicit(&q->released,q->reading,memory_order_release);
    atomic_store_explicit(&q->reader_done,true,memory_order_release);
}
bool ch_reader_finish(ch_queue *q)
{
    if(!q||q->borrowed)return false;
    unsigned end=atomic_load_explicit(&q->terminal,memory_order_acquire);
    uint32_t published=atomic_load_explicit(&q->published,memory_order_acquire);
    if((end==CH_RUNNING&&!atomic_load_explicit(&q->reader_failed,memory_order_acquire))||
       (end==CH_EOF&&q->reading!=published))return false;
    /* Failed session abandons remaining valid chunks; no later frame skip. */
    q->reading=published;atomic_store_explicit(&q->released,published,memory_order_release);
    atomic_store_explicit(&q->reader_done,true,memory_order_release);return true;
}
bool ch_reclaimable(const ch_queue *q)
{return q&&atomic_load_explicit(&q->terminal,memory_order_acquire)!=CH_RUNNING&&
    atomic_load_explicit(&q->reader_done,memory_order_acquire)&&
    atomic_load_explicit(&q->writer_done,memory_order_acquire);}

bool ch_succeeded(const ch_queue *q)
{return ch_reclaimable(q)&&atomic_load_explicit(&q->terminal,memory_order_acquire)==CH_EOF&&
    !atomic_load_explicit(&q->reader_failed,memory_order_acquire);}

static bool CH_HOT stage_guards(const ch_pipe *p)
{
    const ch_lease *x=&p->pending;const uint8_t *s=p->pending_source;
    for(unsigned n=0;n<16;++n)
        if(s[(int)n-16]!=0x6d||s[2U*x->meta.output_pairs+n]!=0x6d)return false;
    return true;
}
static bool CH_HOT finish(ch_pipe *p)
{
    if(!p->dma_pending)return true;
    if(!p->ops.finishDMA(p->context)||!stage_guards(p)||!ch_publish(p->queue,&p->pending))return false;
    p->dma_pending=false;return true;
}
bool ch_pipe_init(ch_pipe *p,ch_queue *q,void *stages,const ch_ops *ops,void *context)
{
    if(!p||!q||!stages||((uintptr_t)stages&63U)||!ops||!ops->filter||!ops->finishDMA||
       !ops->beginDMA||!ops->clearBank||!ops->stopADC||!ops->closeDMA||!ops->cycles)return false;
    *p=(ch_pipe){.queue=q,.ops=*ops,.context=context,.stages=stages};return true;
}
bool CH_HOT ch_pipe_step(ch_pipe *p,unsigned count)
{
    if(!p||p->stopped)return false;
    uint32_t before=p->ops.cycles(p->context);ch_lease next;
    /* No blocking if the reader falls behind: stop instead of overwriting. */
    if(!ch_reserve(p->queue,count,&next))return ch_pipe_stop(p,false),false;
    uint8_t *s=p->stages+p->stage*CH_SLOT_BYTES+64U+((2U*next.meta.output_first)&63U);
    memset(s-16U,0x6d,16U);memset(s+2U*next.meta.output_pairs,0x6d,16U);
    /* Current alternate stage can be filtered while the old stage DMA runs. */
    if(!p->ops.filter(p->context,p->stage,count,s,next.meta.output_pairs)||!finish(p))
        return ch_pipe_stop(p,false),false;
    if(!p->ops.beginDMA(p->context,s,2U*next.meta.output_pairs,next.data))
        return ch_pipe_stop(p,false),false;
    p->pending=next;p->pending_source=s;p->dma_pending=true;p->stage^=1U;
    if(!p->ops.clearBank(p->context))return ch_pipe_stop(p,false),false;
    uint32_t elapsed=p->ops.cycles(p->context)-before;
    if(elapsed>p->max_service_cycles)p->max_service_cycles=elapsed;
    if(elapsed>CH_SERVICE_LIMIT)return ch_pipe_stop(p,false),false;
    return true;
}
bool CH_HOT ch_pipe_boundary(ch_pipe *p,uint32_t combined,uint32_t window,bool context_ok)
{
    if(!p||p->stopped)return false;
    if(!context_ok||window>CH_WINDOW_LIMIT||combined>CH_SERVICE_LIMIT)
        return ch_pipe_stop(p,false),false;
    return true;
}
bool CH_HOT ch_pipe_stop(ch_pipe *p,bool success)
{
    if(!p||p->stopped)return false;
    p->ops.stopADC(p->context);p->stopped=true;
    if(success&&!finish(p))success=false;
    p->dma_idle=p->ops.closeDMA(p->context); /* ALWAYS stop before reuse/free. */
    if(!p->dma_idle)success=false;
    p->dma_pending=false;
    if(p->dma_idle)ch_end_stream(p->queue,success);else ch_fail_active(p->queue);
    return success;
}

bool ch_observe(ch_position *p,uint32_t cycles,uint16_t index)
{
    if(!p||index>=16384U||cycles-p->cycle>=CH_POLL_LIMIT)return false;
    unsigned delta=(index-p->index)&16383U;
    if(p->pairs>UINT64_MAX-delta)return false;
    if(!delta&&cycles-p->changed>=245760U)return false;
    p->pairs+=delta;p->index=index;p->cycle=cycles;
    if(delta)p->changed=cycles;
    return true;
}
