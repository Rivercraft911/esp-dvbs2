/* Preallocated AHB memory-to-memory DMA, using installed Espressif IDF APIs.
 * The hardware descriptor and raw-event definitions are in hal/gdma_ll.h.
 * Source banks stay CPU-owned and unchanged until successful RX EOF. */
#include <stdio.h>
#include "dma.h"

#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_cpu.h"
#include "esp_private/esp_psram_mspi.h"
#include "esp_system.h"
#include "hal/gdma_ll.h"

typedef struct {size_t head,bulk,tail;} rx_packed_plan;
static bool IRAM_ATTR rx_pack_copy_plan(uintptr_t source, uintptr_t out, size_t bytes, rx_packed_plan *plan)
{
    if (bytes > 2U * (RX_UNIT_PAIRS+RX_LATE_PAIRS) || ((source ^ out) & 63U)) return false;
    size_t head = (64U - (out & 63U)) & 63U;
    if (head > bytes) head = bytes;
    size_t bulk = (bytes - head) & ~(size_t)63U;
    *plan = (rx_packed_plan){head, bulk, bytes - head - bulk};
    return true;
}

#define COPY_TIMEOUT 244800U
#define RX_ERRORS (GDMA_LL_EVENT_RX_ERR_EOF | GDMA_LL_EVENT_RX_DESC_ERROR | \
                   GDMA_LL_EVENT_RX_DESC_EMPTY | GDMA_LL_EVENT_RX_L1_FIFO_OVF | \
                   GDMA_LL_EVENT_RX_L1_FIFO_UDF | GDMA_LL_EVENT_RX_L3_FIFO_OVF | \
                   GDMA_LL_EVENT_RX_L3_FIFO_UDF)
#define TX_ERRORS (GDMA_LL_EVENT_TX_DESC_ERROR | GDMA_LL_EVENT_TX_L1_FIFO_OVF | \
                   GDMA_LL_EVENT_TX_L1_FIFO_UDF | GDMA_LL_EVENT_TX_L3_FIFO_OVF | \
                   GDMA_LL_EVENT_TX_L3_FIFO_UDF)

static esp_err_t IRAM_ATTR stop(rx_spill *s)
{
    if (!s->active) return ESP_OK;
    gdma_ll_rx_stop(&GDMA, s->channel);
    gdma_ll_tx_stop(&GDMA, s->channel);
    uint32_t before = esp_cpu_get_cycle_count();
    while (!gdma_ll_rx_is_desc_fsm_idle(&GDMA, s->channel) ||
           !gdma_ll_tx_is_desc_fsm_idle(&GDMA, s->channel))
        if (esp_cpu_get_cycle_count() - before >= COPY_TIMEOUT) return ESP_ERR_TIMEOUT;
    esp_psram_mspi_mb();
    s->active = false;
    return ESP_OK;
}

/* SDK gdma_default_rx_isr clears hardware status before the callback.
 * Preserve the event here; finish must not poll the already-cleared EOF bit.
 * One core owns this stream. Source/destination metadata is not published here. */
static bool IRAM_ATTR notify(rx_spill *s,uint32_t flags,uint32_t descriptor)
{
    s->event_flags|=flags;
    if(flags&GDMA_LL_EVENT_RX_SUC_EOF)s->event_desc=descriptor;
    s->event_cycle=esp_cpu_get_cycle_count();++s->event_count;
    BaseType_t wake=pdFALSE;
    if(xSemaphoreGiveFromISR(s->eof,&wake)!=pdTRUE)++s->notification_errors;
    /* The collector and IDLE share priority0. Force a scheduling decision even
     * when GiveFromISR does not request one for an equal-priority waiter. */
    return true;
}
static bool IRAM_ATTR eof_event(gdma_channel_handle_t channel,gdma_event_data_t *e,void *opaque)
{
    (void)channel;rx_spill *s=opaque;uint32_t flags=0;
    if(e->flags.normal_eof)flags|=GDMA_LL_EVENT_RX_SUC_EOF;
    if(e->flags.abnormal_eof)flags|=GDMA_LL_EVENT_RX_ERR_EOF;
    return notify(s,flags,(uint32_t)e->rx_eof_desc_addr);
}
static bool IRAM_ATTR descriptor_error(gdma_channel_handle_t channel,gdma_event_data_t *e,void *opaque)
{(void)channel;(void)e;return notify(opaque,GDMA_LL_EVENT_RX_DESC_ERROR,0);}
static bool IRAM_ATTR descriptor_empty(gdma_channel_handle_t channel,gdma_event_data_t *e,void *opaque)
{(void)channel;(void)e;return notify(opaque,GDMA_LL_EVENT_RX_DESC_EMPTY,0);}
esp_err_t rx_spill_notifications(rx_spill *s)
{
    if(!s||s->active||s->notifications)return ESP_ERR_INVALID_STATE;
    s->eof=xSemaphoreCreateBinaryStatic(&s->eof_storage);
    if(!s->eof)return ESP_ERR_NO_MEM;
    gdma_rx_event_callbacks_t callbacks={.on_recv_eof=eof_event,.on_descr_err=descriptor_error,
                                       .on_descr_empty=descriptor_empty};
    esp_err_t e=gdma_register_rx_event_callbacks(s->rx,&callbacks,s);
    s->notifications=e==ESP_OK;return e;
}

esp_err_t rx_spill_open(rx_spill *s)
{
    gdma_channel_alloc_config_t channels = {.flags.isr_cache_safe=true};
    esp_err_t e = gdma_new_ahb_channel(&channels, &s->tx, &s->rx);
    if (e != ESP_OK) return e;
    int group, tx_channel;
    if ((e = gdma_get_group_channel_id(s->rx, &group, &s->channel)) != ESP_OK ||
        (e = gdma_get_channel_id(s->tx, &tx_channel)) != ESP_OK) return e;
    if (group != 0 || tx_channel != s->channel) return ESP_ERR_INVALID_STATE;
    uint32_t available;
    if ((e = gdma_get_free_m2m_trig_id_mask(s->tx, &available)) != ESP_OK) return e;
    if (!available) return ESP_ERR_NOT_FOUND;
    gdma_trigger_t trigger = GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_M2M, 0);
    trigger.instance_id = __builtin_ctz(available);
    if ((e = gdma_connect(s->rx, trigger)) != ESP_OK) return e;
    s->rx_connected = true;
    if ((e = gdma_connect(s->tx, trigger)) != ESP_OK) return e;
    s->tx_connected = true;
    gdma_strategy_config_t strategy = {.owner_check = true, .auto_update_desc = true,
                                       .eof_till_data_popped = true};
    gdma_transfer_config_t transfer = {.max_data_burst_size = 64, .access_ext_mem = true};
    if ((e = gdma_apply_strategy(s->tx, &strategy)) != ESP_OK ||
        (e = gdma_apply_strategy(s->rx, &strategy)) != ESP_OK ||
        (e = gdma_config_transfer(s->tx, &transfer)) != ESP_OK ||
        (e = gdma_config_transfer(s->rx, &transfer)) != ESP_OK) return e;
    size_t internal, external;
    if ((e = gdma_get_alignment_constraints(s->tx, &internal, &external)) != ESP_OK) return e;
    if (internal > 64) return ESP_ERR_NOT_SUPPORTED;
    if ((e = gdma_get_alignment_constraints(s->rx, &internal, &external)) != ESP_OK) return e;
    if (external > 64) return ESP_ERR_NOT_SUPPORTED;
    gdma_link_list_config_t links = {.num_items = 16, .item_alignment = 4,
                                     .flags.check_owner = true};
    if ((e = gdma_new_link_list(&links, &s->tx_list)) != ESP_OK ||
        (e = gdma_new_link_list(&links, &s->rx_list)) != ESP_OK) return e;
    return ESP_OK;
}

esp_err_t IRAM_ATTR rx_spill_begin(rx_spill *s, const uint32_t *bank, uint16_t first,
                                  size_t pairs, uint32_t *out)
{
    rx_copy_plan p;
    if (s->active || !rx_plan_copy(first, pairs, &p) ||
        (((uintptr_t)(bank + first) ^ (uintptr_t)out) & 63U)) return ESP_ERR_INVALID_ARG;
    rx_copy(bank, first, p.head, out);
    rx_copy(bank, (first + p.head + p.bulk) & RX_BANK_MASK, p.tail, out + p.head + p.bulk);
    s->bytes = p.bulk * 4U;
    s->out = (uint8_t *)(out + p.head);
    if (!s->bytes) return ESP_OK;
    unsigned at = (first + p.head) & RX_BANK_MASK;
    gdma_buffer_mount_config_t tx[] = {
        {.buffer = (void *)(bank + at), .buffer_alignment = 64, .length = p.split * 4U,
         .flags.mark_eof = p.split == p.bulk,
         .flags.mark_final = p.split == p.bulk ? GDMA_FINAL_LINK_TO_NULL : GDMA_FINAL_LINK_TO_DEFAULT},
        {.buffer = (void *)bank, .buffer_alignment = 64, .length = (p.bulk - p.split) * 4U,
         .flags.mark_eof = true, .flags.mark_final = GDMA_FINAL_LINK_TO_NULL},
    };
    gdma_buffer_mount_config_t rx = {.buffer = s->out, .buffer_alignment = 64, .length = s->bytes,
                                    .flags.mark_final = GDMA_FINAL_LINK_TO_NULL};
    esp_err_t e;
    if ((e = gdma_link_mount_buffers(s->tx_list, 0, tx, 2, NULL)) != ESP_OK ||
        (e = gdma_link_mount_buffers(s->rx_list, 0, &rx, 1, &s->rx_end)) != ESP_OK) return e;
    for (int n = 0; n <= s->rx_end; ++n)
        if ((e = gdma_link_set_length(s->rx_list, n, 0)) != ESP_OK) return e;
    uint32_t before = esp_cpu_get_cycle_count();
    e = esp_cache_msync(s->out, s->bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    uint32_t elapsed = esp_cpu_get_cycle_count() - before;
    if (elapsed > s->max_cache_prepare_cycles) s->max_cache_prepare_cycles = elapsed;
    if (e != ESP_OK || (e = gdma_reset(s->rx)) != ESP_OK ||
        (e = gdma_reset(s->tx)) != ESP_OK) return e;

    if(s->notifications) {
        while(xSemaphoreTake(s->eof,0)==pdTRUE){}
        s->event_flags=s->event_desc=s->event_cycle=s->event_count=0;
    }
    gdma_ll_rx_clear_interrupt_status(&GDMA, s->channel, GDMA_LL_RX_EVENT_MASK);
    gdma_ll_tx_clear_interrupt_status(&GDMA, s->channel, GDMA_LL_TX_EVENT_MASK);
    __asm__ volatile ("memw" ::: "memory");
    s->started_cycle=esp_cpu_get_cycle_count();
    if ((e = gdma_start(s->rx, gdma_link_get_head_addr(s->rx_list))) != ESP_OK) return e;
    s->active = true;
    e = gdma_start(s->tx, gdma_link_get_head_addr(s->tx_list));
    if (e != ESP_OK) stop(s);
    return e;
}

esp_err_t IRAM_ATTR rx_spill_finish(rx_spill *s)
{
    if (!s->bytes) return ESP_OK;
    esp_err_t e = ESP_OK;
    uint32_t before = esp_cpu_get_cycle_count();
    if(s->notifications) {
        bool got=xSemaphoreTake(s->eof,0)==pdTRUE;
        if(!got) {
            uint32_t blocked=esp_cpu_get_cycle_count();++s->blocked_waits;
            got=xSemaphoreTake(s->eof,1)==pdTRUE;
            uint32_t cost=esp_cpu_get_cycle_count()-blocked;
            s->total_blocked_wait_cycles+=cost;
            if(cost>s->max_blocked_wait_cycles)s->max_blocked_wait_cycles=cost;
        }
        if(!got||esp_cpu_get_cycle_count()-before>=COPY_TIMEOUT)e=ESP_ERR_TIMEOUT;
        s->rx_flags=s->event_flags|gdma_ll_rx_get_interrupt_status(&GDMA,s->channel,true);
        s->tx_flags=gdma_ll_tx_get_interrupt_status(&GDMA,s->channel,true);
        if((s->rx_flags&RX_ERRORS)||(s->tx_flags&TX_ERRORS)||s->event_count!=1U||
           !(s->rx_flags&GDMA_LL_EVENT_RX_SUC_EOF)||s->notification_errors)e=ESP_FAIL;
        if(s->event_desc!=gdma_link_get_item_addr(s->rx_list,s->rx_end))e=ESP_FAIL;
        uint32_t transfer=s->event_cycle-s->started_cycle;
        if(transfer>s->max_transfer_cycles)s->max_transfer_cycles=transfer;
    } else {
        do {
            s->rx_flags = gdma_ll_rx_get_interrupt_status(&GDMA, s->channel, true);
            s->tx_flags = gdma_ll_tx_get_interrupt_status(&GDMA, s->channel, true);
            if ((s->rx_flags & RX_ERRORS) || (s->tx_flags & TX_ERRORS)) { e = ESP_FAIL; break; }
            if (esp_cpu_get_cycle_count() - before >= COPY_TIMEOUT) { e = ESP_ERR_TIMEOUT; break; }
        } while (!(s->rx_flags & GDMA_LL_EVENT_RX_SUC_EOF));
    }
    uint32_t waited=esp_cpu_get_cycle_count()-before;
    s->total_wait_cycles+=waited;
    if(waited>s->max_wait_cycles)s->max_wait_cycles=waited;
    if (e == ESP_OK && gdma_ll_rx_get_success_eof_desc_addr(&GDMA, s->channel) !=
                       gdma_link_get_item_addr(s->rx_list, s->rx_end)) e = ESP_FAIL;
    uint32_t completion=esp_cpu_get_cycle_count();
    esp_err_t stopped = stop(s);
    if (stopped != ESP_OK) return stopped;
    if(s->notifications&&(s->event_count!=1U||s->notification_errors))e=ESP_FAIL;
    if (e != ESP_OK) return e;
    size_t total = 0;
    for (int n = 0; n <= s->rx_end; ++n) {
        gdma_lli_owner_t owner;
        if (gdma_link_get_owner(s->rx_list, n, &owner) != ESP_OK || owner != GDMA_LLI_OWNER_CPU)
            return ESP_FAIL;
        size_t length = gdma_link_get_length(s->rx_list, n);
        if (!length || length > 4032U) return ESP_FAIL;
        total += length;
    }
    if (total != s->bytes) return ESP_FAIL;
    before = esp_cpu_get_cycle_count();
    e = esp_cache_msync(s->out, s->bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    uint32_t elapsed = esp_cpu_get_cycle_count() - before;
    if (elapsed > s->max_cache_finish_cycles) s->max_cache_finish_cycles = elapsed;

    uint32_t complete=esp_cpu_get_cycle_count()-completion;
    s->total_completion_cycles+=complete;
    if(complete>s->max_completion_cycles)s->max_completion_cycles=complete;
    return e;
}

esp_err_t rx_spill_close(rx_spill *s)
{
    if (stop(s) != ESP_OK) {
        printf("@{\"error\":\"raw DMA did not stop; rebooting spare\"}\n");
        fflush(stdout);
        esp_restart();
    }
    bool failed = false;
    if (s->tx_connected) failed |= gdma_disconnect(s->tx) != ESP_OK;
    if (s->rx_connected) failed |= gdma_disconnect(s->rx) != ESP_OK;
    if (s->tx) failed |= gdma_del_channel(s->tx) != ESP_OK;
    if (s->rx) failed |= gdma_del_channel(s->rx) != ESP_OK;
    if (s->tx_list) failed |= gdma_del_link_list(s->tx_list) != ESP_OK;
    if (s->rx_list) failed |= gdma_del_link_list(s->rx_list) != ESP_OK;
    return failed ? ESP_FAIL : ESP_OK;
}

esp_err_t IRAM_ATTR rx_spill_packed_begin(rx_spill *s, const uint8_t *stage,
                                         size_t bytes, uint8_t *out)
{
    rx_packed_plan p;
    if (s->active || !rx_pack_copy_plan((uintptr_t)stage, (uintptr_t)out, bytes, &p))
        return ESP_ERR_INVALID_ARG;
    uint32_t before = esp_cpu_get_cycle_count();
    for (size_t n = 0; n < p.head; ++n) out[n] = stage[n];
    for (size_t n = p.head + p.bulk; n < bytes; ++n) out[n] = stage[n];
    uint32_t elapsed = esp_cpu_get_cycle_count() - before;
    if (elapsed > s->max_head_tail_cycles) s->max_head_tail_cycles = elapsed;
    if (!p.bulk) { s->bytes = 0; s->rx_flags = s->tx_flags = 0; return ESP_OK; }
    esp_err_t e=rx_spill_begin(s,(const uint32_t *)(stage+p.head),0,p.bulk/4U,(uint32_t *)(out+p.head));
    elapsed=esp_cpu_get_cycle_count()-before;
    if(elapsed>s->max_submit_cycles)s->max_submit_cycles=elapsed;
    return e;
}

esp_err_t IRAM_ATTR rx_spill_abort(rx_spill *s) { return stop(s); }
