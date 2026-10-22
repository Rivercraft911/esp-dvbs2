#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define CH_HOT IRAM_ATTR
#else
#define CH_HOT
#endif

/* One core1 producer, one core0 reader. Metadata/atomics are INTERNAL;
 * separately allocated64-aligned arena is PSRAM. No RMW, locks or allocation.
 * Each borrowed chunk is immutable until release. Ticket subtraction is modulo
 * 2^32 with at most64 outstanding; sample positions are64-bit. */
enum { CH_MAX_SLOTS=64, CH_SLOT_BYTES=7360, CH_UNIT=12288, CH_LATE=2000,
       CH_SERVICE_LIMIT=154335, CH_WINDOW_LIMIT=23885, CH_POLL_LIMIT=244800 };
typedef struct {uint64_t input_first, output_first;uint32_t input_pairs, output_pairs;} ch_meta;
typedef struct {uint32_t ticket;ch_meta meta;uint8_t *data;} ch_lease;
enum ch_end {CH_RUNNING,CH_EOF,CH_FAILED};
enum ch_read {CH_EMPTY,CH_DATA,CH_END,CH_ERROR};
typedef struct {
    _Alignas(64) _Atomic uint32_t published;
    _Alignas(64) _Atomic uint32_t released;
    _Atomic unsigned terminal;
    _Atomic bool reader_done, writer_done, reader_failed;
    uint8_t *arena;
    unsigned slots;
    /* Producer-only fields. At most current CPU output + previous DMA pending. */
    uint32_t reserved;
    uint64_t input_pairs, output_pairs;
    ch_meta meta[CH_MAX_SLOTS];
    /* Consumer-only fields. */
    uint32_t reading;
    bool borrowed;
} ch_queue;

/* Only before either task starts. ticket_seed0 in a real stream; a nonzero seed
 * exercises rollover without processing2^32 chunks. Arena must be disjoint. */
bool ch_init(ch_queue *,void *arena,size_t bytes,unsigned slots,uint32_t ticket_seed);
bool ch_reserve(ch_queue *,unsigned input_pairs,ch_lease *); /* producer only */
bool ch_publish(ch_queue *,const ch_lease *); /* AFTER DMAfinish/cache/guards */
void ch_end_stream(ch_queue *,bool success); /* AFTER all DMA/CPU writes stop */
void ch_fail_active(ch_queue *); /* reader stop request; NOT permission to free */
enum ch_read ch_acquire(ch_queue *,ch_lease *); /* reader; does not copy IQ */
bool ch_release(ch_queue *,const ch_lease *);
void ch_reader_abort(ch_queue *); /* relinquishes all IQ pointers on failed session */
void ch_reader_reject(ch_queue *); /* reader requests producer stop, release borrow first */
bool ch_reader_finish(ch_queue *); /* EOF drained, or failed stream abandoned */
bool ch_guards(const ch_queue *,const ch_lease *);
bool ch_reclaimable(const ch_queue *);
bool ch_succeeded(const ch_queue *); /* only AFTER both producer/reader finish */ /* no task may touch arena afterwards */

/* Collector adapter to the unchanged FIR/spill/clear calls. Calls are
 * synchronous except beginDMA, whose two buffers stay live until finish/close.
 * filter() includes original complete sentinel prescan and persistent state.
 * finishDMA() includes original EOF/owner/length/cache validation.
 * closeDMA() MUST prove idle, else all storage remains quarantined.
 * stopADC() stops engine/ownership first; no raw-bank reuse by core0. */
typedef struct {
    bool (*filter)(void *,unsigned stage,unsigned count,uint8_t *out,unsigned expected);
    bool (*finishDMA)(void *);
    bool (*beginDMA)(void *,const uint8_t *,unsigned,uint8_t *);
    bool (*clearBank)(void *);
    void (*stopADC)(void *);
    bool (*closeDMA)(void *);
    uint32_t (*cycles)(void *);
} ch_ops;
typedef struct {
    ch_queue *queue;ch_ops ops;void *context;
    uint8_t *stages; /* two original7360-byte stages, not the FIR scratch */
    ch_lease pending;
    uint8_t *pending_source;
    unsigned stage;
    bool dma_pending, stopped, dma_idle;
    uint32_t max_service_cycles;
} ch_pipe;
bool ch_pipe_init(ch_pipe *,ch_queue *,void *two_stages,const ch_ops *,void *);
bool ch_pipe_step(ch_pipe *,unsigned count);
/* Gate measured CPU-unit+admission wall cost and context restoration. This
 * function does not mask interrupts. The collector yields at priority0 with
 * an explicit idle-hook return, then observes a fresh hardware index. */
bool ch_pipe_boundary(ch_pipe *,uint32_t combined,uint32_t window,bool context_ok);
bool ch_pipe_stop(ch_pipe *,bool success);

/* Exact original advance arithmetic, except long-running pair/target epochs
 * are64-bit. MMIO cycle/index reads remain caller-owned and fresh after IRQs. */
typedef struct {uint64_t pairs,target;uint32_t cycle,changed;uint16_t index;} ch_position;
bool ch_observe(ch_position *,uint32_t cycles,uint16_t index);
