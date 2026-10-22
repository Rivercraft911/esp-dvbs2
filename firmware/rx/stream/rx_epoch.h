#pragma once
#include <stdbool.h>
#include <stdint.h>

/* One producer observes CCOUNT at bank handoffs. A tick gap of ten seconds
 * is rejected before an unsigned delta could hide a full 17.9-second wrap. */
typedef struct {uint64_t total;uint32_t last,ticks;bool valid;} stream_epoch;
static inline __attribute__((always_inline)) void stream_epoch_init(stream_epoch *e,uint32_t now,uint32_t ticks)
{*e=(stream_epoch){0,now,ticks,true};}
static inline __attribute__((always_inline)) bool stream_epoch_add(stream_epoch *e,uint32_t now,uint32_t ticks)
{
    if(!e->valid)return false;
    if((uint32_t)(ticks-e->ticks)>=1000U){e->valid=false;return false;}
    e->total+=(uint32_t)(now-e->last);e->last=now;e->ticks=ticks;
    return true;
}
static inline __attribute__((always_inline)) bool stream_epoch_peek(const stream_epoch *e,uint32_t now,uint64_t *total)
{
    if(!e->valid)return false;
    *total=e->total+(uint32_t)(now-e->last);
    return true;
}
