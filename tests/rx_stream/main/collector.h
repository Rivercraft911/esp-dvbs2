#pragma once
#include "esp_err.h"
#include "samples.h"
#include "controls.h"

#ifndef RX_GAIN_REFRESH_CYCLES
#define RX_GAIN_REFRESH_CYCLES 0U
#endif

typedef struct {
    uint64_t retained_pairs, acquired_pairs;
    uint32_t units;
    uint32_t late, overruns, boundary_errors, gaps, sentinel_errors;
    uint32_t max_poll_cycles, max_copy_cycles, max_switch_late_pairs;
    uint32_t dma_errors, dma_copies, dma_rx_flags, dma_tx_flags;
    uint64_t engine_cycles, elapsed_cycles;
    uint32_t snapshot_pairs, snapshot_crc32;
    uint8_t gain_max;
    esp_err_t error, cleanup_error;
    rx_lo_plan tuning;
    uint8_t lo_saved, lo_active, lo_restored;
    bool lo_applied, lo_restore_ok;
    uint32_t max_pack_cycles;
    uint32_t stage_bytes, stage_free_internal, stage_largest_internal;
    uint32_t ready_free_internal, ready_largest_internal, ready_free_psram, ready_largest_psram;
    bool ready_memory_measured;
    bool stage_guards_ok;
    uint64_t output_pairs, committed_output_pairs;
    uint32_t pending_output_bytes;
    uint32_t filter_deadline_errors, final_drain_cycles, engine_timeouts;
    uint32_t stage_free_after, stage_largest_after;
    bool filter_drained, output_guards_ok, stage_allocated, stage_freed;
    bool tick_hook_registered, tick_hook_deregistered;
    uint32_t worker_core, irq_saved_level, irq_restored_level;
    uint32_t ticks_at_start, ticks_at_engine_stop, ticks_before_unmask;
    uint32_t irq_windows, max_irq_window_cycles, irq_window_budget_errors;
    uint32_t max_unit_with_window_cycles, irq_unit_budget_errors, irq_context_errors;
    hb_context_report leaf_context;
    uint32_t max_protected_leaf_cycles,protected_leaf_calls,worker_stack_free;
    uint32_t max_transfer_cycles,max_wait_cycles,max_blocked_wait_cycles,max_completion_cycles;
    uint32_t max_submit_cycles,blocked_waits,notification_errors;
    uint64_t total_wait_cycles,total_blocked_wait_cycles,total_completion_cycles;
} rx_report;

#include "rx_queue.h"
#include "rx_epoch.h"

/* Finite capture profile selected at build time; no console duration input. */
#define STREAM_SINK_YIELD_US 200000U
#ifndef RX_STREAM_UNITS
#define RX_STREAM_UNITS 40960U
#endif
#define STREAM_UNITS RX_STREAM_UNITS
#define STREAM_INPUT_RATE 16000000U
#define STREAM_NOMINAL_US ((UINT64_C(1000000)*STREAM_UNITS*CH_UNIT)/STREAM_INPUT_RATE)
#define STREAM_CHECKPOINT_UNITS ((STREAM_UNITS+4U)/5U)
#define STREAM_CHECKPOINTS 5U
#if RX_STREAM_UNITS == 8192U
#define STREAM_MIN_CYCLES UINT64_C(1200000000)
#define STREAM_MIN_TICKS 600U
#define STREAM_LIMIT_CYCLES UINT64_C(1584000000)
#elif RX_STREAM_UNITS == 40960U
#define STREAM_MIN_CYCLES UINT64_C(7200000000)
#define STREAM_MIN_TICKS 3000U
#define STREAM_LIMIT_CYCLES UINT64_C(7920000000)
#elif RX_STREAM_UNITS == 390625U
/* Nominal300s, 4.8billion raw pairs. Original per-unit deadlines unchanged. */
#define STREAM_MIN_CYCLES UINT64_C(68400000000)
#define STREAM_MIN_TICKS 28500U
#define STREAM_LIMIT_CYCLES UINT64_C(75600000000)
#else
#error "Use only the reviewed8192,40960,390625-unit profiles"
#endif
typedef struct {
    uint64_t elapsed_cycles;
    uint64_t retained_pairs;
    uint32_t units,reserved,published,released,queued,interval_max_queued;
    bool epoch_valid;
} stream_checkpoint;
typedef struct rx_stream_report {
    rx_report capture;
    rrc_partial_report partial;
    bool partial_passed;
    uint64_t partial_elapsed_us;
    uint32_t partial_yields,partial_stack_free,partial_core;
    uint8_t *matched_allocation,*matched_prefix;
    uint64_t matched_pairs;
    uint32_t matched_prefix_pairs,matched_prefix_crc,rrc_steps,max_rrc_mask_cycles;
    uint32_t consumer_stack_free,rrc_free_internal_before,rrc_largest_internal_before,rrc_free_internal_after;
    bool rrc_allocated,rrc_freed,rrc_guards,rrc_context_ok,matched_prefix_guards;
    uint8_t *saved_allocation,*saved;
    ch_meta sample[2];
    uint32_t sample_crc[2],sample_bytes[2];
    uint32_t consumed_chunks,consumed_crc,consumer_errors,consumer_yields;
    uint64_t consumed_inputs,consumed_outputs;
    uint64_t sink_elapsed_us,sink_work_elapsed_us,sink_wait_elapsed_us;
    uint64_t max_sink_work_segment_us,max_sink_wait_us;
    bool sink_timing_measured;
    uint32_t max_sink_us,max_sink_gap_us,max_queued_chunks;
    stream_checkpoint checkpoint[STREAM_CHECKPOINTS];
    uint32_t checkpoint_count,interval_max_queued,epoch_errors,max_epoch_tick_gap;
    bool epoch_valid;
    uint32_t idle_visits,consumer_core;
    bool consumer_joined,queue_freed,arena_freed,saved_guards,complete;
} rx_stream_report;
esp_err_t rx_stream(uint16_t mhz,uint8_t gain,uint32_t snapshot[512],rx_stream_report *);
void rx_stream_release(rx_stream_report *);

/* Radio-off ABI admission before any RF setup. */
esp_err_t rx_leafcheck(rx_report *);
