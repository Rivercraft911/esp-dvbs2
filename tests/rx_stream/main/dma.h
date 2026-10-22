#pragma once
#include "esp_private/gdma.h"
#include "esp_private/gdma_link.h"
#include "samples.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    gdma_channel_handle_t tx, rx;
    gdma_link_list_handle_t tx_list, rx_list;
    int channel, rx_end;
    size_t bytes;
    uint8_t *out;
    uint32_t rx_flags, tx_flags;
    bool tx_connected, rx_connected, active;
    uint32_t max_head_tail_cycles, max_cache_prepare_cycles, max_cache_finish_cycles;
    StaticSemaphore_t eof_storage;
    SemaphoreHandle_t eof;
    volatile uint32_t event_flags,event_desc,event_cycle,event_count;
    uint32_t started_cycle,max_transfer_cycles,max_wait_cycles,max_blocked_wait_cycles;
    uint32_t max_completion_cycles,max_submit_cycles,blocked_waits,notification_errors;
    uint64_t total_wait_cycles,total_blocked_wait_cycles,total_completion_cycles;
    bool notifications;
} rx_spill;

esp_err_t rx_spill_open(rx_spill *spill);
esp_err_t rx_spill_begin(rx_spill *spill, const uint32_t *bank, uint16_t first,
                         size_t pairs, uint32_t *out);
/* Contiguous internal SC8 stage, matching source/destination phase modulo64.
 * CPU copies partial cachelines; the existing raw DMA handles aligned bulk. */
esp_err_t rx_spill_packed_begin(rx_spill *spill, const uint8_t *stage,
                                size_t bytes, uint8_t *out);
esp_err_t rx_spill_finish(rx_spill *spill);
esp_err_t rx_spill_close(rx_spill *spill);

/* Stop/idle proof without deleting handles; capture teardown closes later. */
esp_err_t rx_spill_abort(rx_spill *spill);

/* Called once on the collector core, before ADC start. No dynamic semaphore. */
esp_err_t rx_spill_notifications(rx_spill *);
