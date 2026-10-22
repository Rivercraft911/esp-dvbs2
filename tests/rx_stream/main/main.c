/* Fixed receive-only chunk probe. All console work follows RF/DMA teardown. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "collector.h"
#include "console.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "esp_private/esp_clk.h"

static struct {uint32_t before[4],raw[512],after[4];} snapshot;
static rx_stream_report last;
static bool valid;
static bool snapshot_guards(void)
{
    for(unsigned n=0;n<4;++n)
        if(snapshot.before[n]!=0x6d6d6d6dU||snapshot.after[n]!=0x6d6d6d6dU)return false;
    return true;
}
static bool qualified(void)
{
    rx_report *s=&last.capture;
    return last.partial_passed&&last.partial.measured&&last.partial.cases==RRC_PARTIAL_CASES&&
        last.partial.components==RRC_PARTIAL_COMPONENTS&&last.partial.crc32==RRC_PARTIAL_EXPECTED_CRC&&
        !last.partial.errors&&last.partial.guards&&last.partial.context&&last.partial.reset&&
        last.partial_core==0&&last.partial_yields==RRC_PARTIAL_CASES&&last.partial_stack_free>=512U&&
        s->leaf_context.complete&&s->leaf_context.cases==72U&&s->leaf_context.forced_calls==60U&&!s->leaf_context.errors&&
        s->max_protected_leaf_cycles>0&&s->max_protected_leaf_cycles<=23885U&&
        s->protected_leaf_calls>0&&s->worker_stack_free>=512U&&!s->notification_errors&&
        last.complete&&s->error==ESP_OK&&s->cleanup_error==ESP_OK&&snapshot_guards()&&
        s->units==STREAM_UNITS&&s->retained_pairs==s->acquired_pairs&&
        s->output_pairs==(s->retained_pairs-27U)/4U&&s->output_pairs==s->committed_output_pairs&&
        !s->pending_output_bytes&&s->filter_drained&&s->dma_copies==STREAM_UNITS&&
        s->stage_allocated&&s->stage_freed&&s->stage_guards_ok&&s->output_guards_ok&&
        !s->late&&!s->overruns&&!s->boundary_errors&&!s->gaps&&!s->sentinel_errors&&!s->dma_errors&&
        !s->filter_deadline_errors&&!s->engine_timeouts&&!s->irq_window_budget_errors&&
        !s->irq_unit_budget_errors&&!s->irq_context_errors&&
        s->max_unit_with_window_cycles>0&&s->max_unit_with_window_cycles<=154335U&&
        s->max_poll_cycles>0&&s->max_poll_cycles<244800U&&
        s->max_switch_late_pairs<2000U&&s->max_irq_window_cycles>0&&s->max_irq_window_cycles<=23885U&&
        last.epoch_valid&&!last.epoch_errors&&last.checkpoint_count==STREAM_CHECKPOINTS&&
        last.checkpoint[STREAM_CHECKPOINTS-1U].units==STREAM_UNITS&&
        s->engine_cycles>STREAM_MIN_CYCLES&&s->engine_cycles<=s->elapsed_cycles&&s->elapsed_cycles<STREAM_LIMIT_CYCLES&&
        s->irq_windows==STREAM_UNITS-1U&&s->worker_core==1&&last.consumer_core==0&&
        s->tick_hook_registered&&s->tick_hook_deregistered&&
        s->ticks_at_engine_stop-s->ticks_at_start>=STREAM_MIN_TICKS&&
        s->ticks_before_unmask>=s->ticks_at_engine_stop&&
        s->irq_saved_level==0&&s->irq_restored_level==0&&last.idle_visits>0&&
        last.consumer_yields>0&&last.consumer_joined&&last.queue_freed&&last.arena_freed&&
        s->lo_applied&&s->lo_restore_ok&&s->snapshot_pairs==512&&
        last.sample_bytes[0]>0&&last.sample_bytes[1]>0&&
        last.rrc_allocated&&last.rrc_freed&&last.rrc_guards&&last.rrc_context_ok&&
        last.consumer_stack_free>=512U&&last.matched_prefix_guards&&last.matched_prefix_pairs==128U;
}
static void cycles_text(char text[32],bool valid,uint64_t cycles)
{
    if(valid)snprintf(text,32,"%llu",(unsigned long long)cycles);
    else memcpy(text,"null",5);
}
static void leaf_result(void)
{
    rx_report *s=&last.capture;
    hb_context_report *c=&s->leaf_context;
    printf("@{\"cmd\":\"stream_leaf_context\",\"complete\":%s,\"cases\":%lu,\"forced_window_spills\":%lu,\"errors\":%lu,"
           "\"first_leaf\":%lu,\"first_trial\":%lu,\"max_protected_leaf_cycles\":%lu,\"calls\":%lu,\"worker_stack_free\":%lu,"
           "\"QR_QACC_ACCX_SAR_SAR_BYTE_CPE_restored\":%s,\"PS_except_ABI_CALLINC_OWB_restored\":%s,"
           "\"raw_difference_cases\":%lu,\"PS_excluded_mask\":200448,\"first_difference_includes_ABI\":true}\n",
           s->leaf_context.complete?"true":"false",(unsigned long)s->leaf_context.cases,(unsigned long)s->leaf_context.forced_calls,
           (unsigned long)s->leaf_context.errors,(unsigned long)s->leaf_context.first_leaf,
           (unsigned long)s->leaf_context.first_trial,(unsigned long)s->max_protected_leaf_cycles,
           (unsigned long)s->protected_leaf_calls,(unsigned long)s->worker_stack_free,
           !c->mismatch_words[0]&&!(c->mismatch_words[1]&0x007fffffU)?"true":"false",
           !(c->xor_words[55]&~((3U<<16)|(15U<<8)))?"true":"false",(unsigned long)c->raw_difference_cases);
    for(unsigned n=0;n<c->cases&&n<HB_CONTEXT_CASES;++n)
        if(c->case_words[n][0]||c->case_words[n][1])
            printf("@{\"cmd\":\"leaf_context_diff\",\"case\":%u,\"leaf\":%u,\"trial\":%u,"
                   "\"word_mask_lo\":%lu,\"word_mask_hi\":%lu}\n",n,n/12U,n%12U,
                   (unsigned long)c->case_words[n][0],(unsigned long)c->case_words[n][1]);
    for(unsigned n=0;n<HB_CONTEXT_WORDS;++n)
        if(c->xor_words[n])
            printf("@{\"cmd\":\"leaf_context_word\",\"word\":%u,\"first_before\":%lu,\"first_after\":%lu,"
                   "\"raw_xor_all\":%lu,\"compared_xor_all\":%lu}\n",n,
                   (unsigned long)c->first_before[n],(unsigned long)c->first_after[n],
                   (unsigned long)c->xor_words[n],(unsigned long)(c->xor_words[n]&(n==55U?~((3U<<16)|(15U<<8)):UINT32_MAX)));
}
static void report(void)
{
    rx_report *s=&last.capture;
    char engine_text[32],elapsed_text[32];
    cycles_text(engine_text,last.epoch_valid,s->engine_cycles);
    cycles_text(elapsed_text,last.epoch_valid,s->elapsed_cycles);
    printf("@{\"cmd\":\"stream\",\"complete\":%s,\"status\":\"%s\",\"cleanup_status\":\"%s\","
           "\"units\":%lu,\"target_units\":%u,\"input_rate_sps\":16000000,\"output_rate_sps\":4000000,"
           "\"input_pairs\":%llu,\"output_pairs\":%llu,\"committed_output_pairs\":%llu,\"pending_output_bytes\":%lu,"
           "\"sink_chunks\":%lu,\"sink_inputs\":%llu,\"sink_outputs\":%llu,\"sink_crc32\":null,\"input_whole_crc32\":null,\"matched_crc32\":%lu,"
           "\"sink_errors\":%lu,\"sink_yields\":%lu,\"max_sink_us\":%lu,\"max_sink_gap_us\":%lu,"
           "\"max_queued_chunks\":%lu,\"queue_slots\":64,\"slot_bytes\":7360,\"ring_bytes\":471040,"
           "\"core1_idle_visits_during_ADC\":%lu,\"collector_core\":%lu,\"sink_core\":%lu,"
           "\"consumer_joined\":%s,\"queue_freed\":%s,\"arena_freed\":%s,"
           "\"max_service_cycles\":%lu,\"max_service_with_admission_cycles\":%lu,\"max_poll_cycles\":%lu,"
           "\"max_irq_window_cycles\":%lu,\"irq_windows\":%lu,\"max_late_pairs\":%lu,"
           "\"late\":%lu,\"overruns\":%lu,\"boundary_errors\":%lu,\"gaps\":%lu,\"sentinel_errors\":%lu,"
           "\"dma_errors\":%lu,\"dma_copies\":%lu,\"filter_deadline_errors\":%lu,\"engine_timeouts\":%lu,"
           "\"irq_window_budget_errors\":%lu,\"irq_unit_budget_errors\":%lu,\"irq_context_errors\":%lu,"
           "\"ticks_at_start\":%lu,\"ticks_at_engine_stop\":%lu,\"ticks_before_unmask\":%lu,"
           "\"engine_cycles\":%s,\"elapsed_cycles\":%s,\"filter_drained\":%s,"
           "\"stage_allocated\":%s,\"stage_freed\":%s,\"stage_guards_ok\":%s,\"saved_guards\":%s,"
           "\"snapshot_pairs\":%lu,\"snapshot_crc32\":%lu,\"snapshot_guards\":%s,"
           "\"requested_rf_khz\":%lu,\"requested_pll_khz\":%lu,\"lo_mode\":\"%s\","
           "\"lo_selector_saved\":%u,\"lo_selector_active\":%u,\"lo_selector_restored\":%u,"
           "\"lo_applied\":%s,\"lo_restore_ok\":%s,\"decoded_payload_qualified\":false}\n",
           valid?"true":"false",esp_err_to_name(s->error),esp_err_to_name(s->cleanup_error),
           (unsigned long)s->units,(unsigned)STREAM_UNITS,(unsigned long long)s->retained_pairs,(unsigned long long)s->output_pairs,
           (unsigned long long)s->committed_output_pairs,(unsigned long)s->pending_output_bytes,
           (unsigned long)last.consumed_chunks,(unsigned long long)last.consumed_inputs,(unsigned long long)last.consumed_outputs,
           (unsigned long)last.consumed_crc,(unsigned long)last.consumer_errors,(unsigned long)last.consumer_yields,
           (unsigned long)last.max_sink_us,(unsigned long)last.max_sink_gap_us,(unsigned long)last.max_queued_chunks,
           (unsigned long)last.idle_visits,(unsigned long)s->worker_core,(unsigned long)last.consumer_core,
           last.consumer_joined?"true":"false",last.queue_freed?"true":"false",last.arena_freed?"true":"false",
           (unsigned long)s->max_copy_cycles,(unsigned long)s->max_unit_with_window_cycles,(unsigned long)s->max_poll_cycles,
           (unsigned long)s->max_irq_window_cycles,(unsigned long)s->irq_windows,(unsigned long)s->max_switch_late_pairs,
           (unsigned long)s->late,(unsigned long)s->overruns,(unsigned long)s->boundary_errors,(unsigned long)s->gaps,
           (unsigned long)s->sentinel_errors,(unsigned long)s->dma_errors,(unsigned long)s->dma_copies,
           (unsigned long)s->filter_deadline_errors,(unsigned long)s->engine_timeouts,
           (unsigned long)s->irq_window_budget_errors,(unsigned long)s->irq_unit_budget_errors,(unsigned long)s->irq_context_errors,
           (unsigned long)s->ticks_at_start,(unsigned long)s->ticks_at_engine_stop,(unsigned long)s->ticks_before_unmask,
           engine_text,elapsed_text,s->filter_drained?"true":"false",
           s->stage_allocated?"true":"false",s->stage_freed?"true":"false",s->stage_guards_ok?"true":"false",
           last.saved_guards?"true":"false",(unsigned long)s->snapshot_pairs,(unsigned long)s->snapshot_crc32,
           snapshot_guards()?"true":"false",(unsigned long)s->tuning.rf_khz,(unsigned long)s->tuning.pll_khz,
           s->tuning.divided?"five-sixths":"direct",s->lo_saved,s->lo_active,s->lo_restored,
           s->lo_applied?"true":"false",s->lo_restore_ok?"true":"false");
    printf("@{\"cmd\":\"stream_epoch\",\"valid\":%s,\"errors\":%lu,\"max_tick_gap\":%lu,"
           "\"checkpoint_count\":%lu,\"CCOUNT_bits\":32,\"total_bits\":64,\"ambiguity_tick_gap_limit\":1000,"
           "\"engine_limit_cycles\":%llu,\"TWDT_panic\":false}\n",
           last.epoch_valid?"true":"false",(unsigned long)last.epoch_errors,
           (unsigned long)last.max_epoch_tick_gap,(unsigned long)last.checkpoint_count,
           (unsigned long long)STREAM_LIMIT_CYCLES);
    for(unsigned n=0;n<last.checkpoint_count&&n<STREAM_CHECKPOINTS;++n) {
        const stream_checkpoint *c=&last.checkpoint[n];char text[32];
        cycles_text(text,c->epoch_valid,c->elapsed_cycles);
        printf("@{\"cmd\":\"stream_checkpoint\",\"slot\":%u,\"units_observed\":%lu,"
               "\"retained_native_pairs\":%llu,\"elapsed_cycles\":%s,\"epoch_valid\":%s,"
               "\"reserved\":%lu,\"published\":%lu,\"released\":%lu,\"queued\":%lu,"
               "\"interval_max_queued\":%lu}\n",n,(unsigned long)c->units,
               (unsigned long long)c->retained_pairs,text,c->epoch_valid?"true":"false",
               (unsigned long)c->reserved,(unsigned long)c->published,(unsigned long)c->released,
               (unsigned long)c->queued,(unsigned long)c->interval_max_queued);
    }
    printf("@{\"cmd\":\"stream_resources\",\"format\":\"sc8\",\"order\":\"high10=I,low10=Q\","
           "\"scaling\":\"two Q15 halfbands, valid windows, signed10 to sc8\","
           "\"stage_bytes\":%lu,\"stage_free_internal\":%lu,\"stage_largest_internal\":%lu,"
           "\"stage_free_after\":%lu,\"stage_largest_after\":%lu,\"max_filter_cycles\":%lu,"
           "\"final_drain_cycles\":%lu,\"dma_rx_flags\":%lu,\"dma_tx_flags\":%lu,"
           "\"irq_saved_level\":%lu,\"irq_restored_level\":%lu,\"tick_hook_registered\":%s,\"tick_hook_deregistered\":%s}\n",
           (unsigned long)s->stage_bytes,(unsigned long)s->stage_free_internal,(unsigned long)s->stage_largest_internal,
           (unsigned long)s->stage_free_after,(unsigned long)s->stage_largest_after,(unsigned long)s->max_pack_cycles,
           (unsigned long)s->final_drain_cycles,(unsigned long)s->dma_rx_flags,(unsigned long)s->dma_tx_flags,
           (unsigned long)s->irq_saved_level,(unsigned long)s->irq_restored_level,
           s->tick_hook_registered?"true":"false",s->tick_hook_deregistered?"true":"false");
    if(s->ready_memory_measured)
        printf("@{\"cmd\":\"stream_memory\",\"measured\":true,\"phase\":\"ready_before_ADC\","
               "\"free_internal_8bit\":%lu,\"largest_internal_8bit\":%lu,"
               "\"free_psram_8bit\":%lu,\"largest_psram_8bit\":%lu}\n",
               (unsigned long)s->ready_free_internal,(unsigned long)s->ready_largest_internal,
               (unsigned long)s->ready_free_psram,(unsigned long)s->ready_largest_psram);
    else
        puts("@{\"cmd\":\"stream_memory\",\"measured\":false,\"phase\":\"ready_before_ADC\","
             "\"free_internal_8bit\":null,\"largest_internal_8bit\":null,"
             "\"free_psram_8bit\":null,\"largest_psram_8bit\":null}");
    leaf_result();
    printf("@{\"cmd\":\"stream_dma_timing\",\"max_submit_cycles\":%lu,\"max_start_to_ISR_cycles\":%lu,"
           "\"max_finish_wait_cycles\":%lu,\"max_blocked_wait_cycles\":%lu,\"blocked_waits\":%lu,"
           "\"max_completion_cycles\":%lu,\"total_wait_cycles\":%llu,\"total_blocked_wait_cycles\":%llu,"
           "\"total_completion_cycles\":%llu,\"notification_errors\":%lu,"
           "\"scope\":\"Wall cycles include ISR/preemption. Transfer ends at EOF callback; completion includes idle/owner/length/cache checks.\"}\n",
           (unsigned long)s->max_submit_cycles,(unsigned long)s->max_transfer_cycles,
           (unsigned long)s->max_wait_cycles,(unsigned long)s->max_blocked_wait_cycles,
           (unsigned long)s->blocked_waits,(unsigned long)s->max_completion_cycles,
           (unsigned long long)s->total_wait_cycles,(unsigned long long)s->total_blocked_wait_cycles,
           (unsigned long long)s->total_completion_cycles,(unsigned long)s->notification_errors);
    for(unsigned n=0;n<2;++n)printf("@{\"cmd\":\"retained_chunk\",\"slot\":%u,\"input_first\":%llu,"
        "\"output_first\":%llu,\"input_pairs\":%lu,\"output_pairs\":%lu,\"bytes\":%lu,\"crc32\":%lu}\n",n,
        (unsigned long long)last.sample[n].input_first,(unsigned long long)last.sample[n].output_first,
        (unsigned long)last.sample[n].input_pairs,(unsigned long)last.sample[n].output_pairs,
        (unsigned long)last.sample_bytes[n],(unsigned long)last.sample_crc[n]);
    printf("@{\"cmd\":\"stream_rrc\",\"phase\":0,\"input_rate_sps\":4000000,\"output_rate_sps\":1000000,"
           "\"input_pairs\":%llu,\"output_pairs\":%llu,\"output_crc32\":%lu,\"input_whole_crc32\":null,"
           "\"format\":\"s16le\",\"order\":\"I,Q\",\"fractional_bits\":6,\"valid_window_pairs\":65,"
           "\"rounding\":\"floor((Q15dot+256)/512)\",\"prefix_pairs\":%lu,\"prefix_bytes\":%lu,\"prefix_crc32\":%lu,"
           "\"steps\":%lu,\"max_mask_cycles\":%lu,\"max_mask_cycles_scope\":\"wrapper elapsed includes possible post-unmask ISR/preemption\",\"allocated\":%s,\"freed\":%s,\"guards\":%s,\"context_ok\":%s,"
           "\"prefix_guards\":%s,\"internal_allocation_bytes\":5520,\"state_bytes\":4816,\"output_buffer_bytes\":512,"
           "\"prefix_PSRAM_allocation_bytes\":640,\"free_internal_before\":%lu,\"largest_internal_before\":%lu,\"free_internal_after\":%lu,"
           "\"consumer_stack_free_bytes\":%lu,\"yield_interval_us\":200000,\"acquisition_qualified\":false,\"decoded_payload_qualified\":false}\n",
           (unsigned long long)last.consumed_outputs,(unsigned long long)last.matched_pairs,(unsigned long)last.consumed_crc,
           (unsigned long)last.matched_prefix_pairs,(unsigned long)(4U*last.matched_prefix_pairs),(unsigned long)last.matched_prefix_crc,
           (unsigned long)last.rrc_steps,(unsigned long)last.max_rrc_mask_cycles,last.rrc_allocated?"true":"false",last.rrc_freed?"true":"false",
           last.rrc_guards?"true":"false",last.rrc_context_ok?"true":"false",last.matched_prefix_guards?"true":"false",
           (unsigned long)last.rrc_free_internal_before,(unsigned long)last.rrc_largest_internal_before,(unsigned long)last.rrc_free_internal_after,
           (unsigned long)last.consumer_stack_free);
    if(last.partial.measured) {
        char mismatch[160];
        if(last.partial.errors)
            snprintf(mismatch,sizeof mismatch,"{\"case\":%lu,\"component\":%lu,\"expected\":%ld,\"actual\":%ld}",
                     (unsigned long)last.partial.first_case,(unsigned long)last.partial.first_component,
                     (long)last.partial.expected,(long)last.partial.actual);
        else memcpy(mismatch,"null",5);
        printf("@{\"cmd\":\"stream_rrc_controls\",\"measured\":true,\"complete\":%s,\"cases\":%lu,"
               "\"components\":%lu,\"S16LE_crc32\":%lu,\"expected_crc32\":%lu,\"errors\":%lu,"
               "\"guards\":%s,\"context_ok\":%s,\"reset\":%s,\"core\":%lu,\"yields\":%lu,"
               "\"elapsed_us\":%llu,\"stack_free_bytes\":%lu,\"first_mismatch\":%s,"
               "\"scope\":\"pre-ADC controls; excluded from stream timing\"}\n",
               last.partial_passed?"true":"false",(unsigned long)last.partial.cases,
               (unsigned long)last.partial.components,(unsigned long)last.partial.crc32,
               (unsigned long)RRC_PARTIAL_EXPECTED_CRC,(unsigned long)last.partial.errors,
               last.partial.guards?"true":"false",last.partial.context?"true":"false",last.partial.reset?"true":"false",
               (unsigned long)last.partial_core,(unsigned long)last.partial_yields,
               (unsigned long long)last.partial_elapsed_us,(unsigned long)last.partial_stack_free,mismatch);
    } else puts("@{\"cmd\":\"stream_rrc_controls\",\"measured\":false,\"complete\":false,\"cases\":null,\"components\":null,\"S16LE_crc32\":null,\"errors\":null}");
    if(last.sink_timing_measured)
        printf("@{\"cmd\":\"stream_sink_timing\",\"measured\":true,\"elapsed_us\":%llu,"
               "\"work_elapsed_us\":%llu,\"wait_elapsed_us\":%llu,\"wait_calls\":%lu,"
               "\"max_work_segment_us\":%llu,\"max_wait_us\":%llu,\"max_chunk_gap_us\":%lu,"
               "\"cooperation_interval_us\":200000,\"scope\":\"post-preflight reader loop through final guards before notification; work excludes bracketed vTaskDelay elapsed and includes ISR/preemption\"}\n",
               (unsigned long long)last.sink_elapsed_us,(unsigned long long)last.sink_work_elapsed_us,
               (unsigned long long)last.sink_wait_elapsed_us,(unsigned long)last.consumer_yields,
               (unsigned long long)last.max_sink_work_segment_us,(unsigned long long)last.max_sink_wait_us,
               (unsigned long)last.max_sink_gap_us);
    else puts("@{\"cmd\":\"stream_sink_timing\",\"measured\":false,\"elapsed_us\":null,\"work_elapsed_us\":null,\"wait_elapsed_us\":null,\"wait_calls\":null,\"max_work_segment_us\":null,\"max_wait_us\":null,\"max_chunk_gap_us\":null,\"cooperation_interval_us\":200000}");

}
static void download(const void *data,size_t bytes,uint32_t crc,const char *cmd)
{
    printf("@{\"cmd\":\"%s\",\"binary_bytes\":%u,\"crc32\":%lu}\n",cmd,(unsigned)bytes,(unsigned long)crc);
    for(size_t at=0;at<bytes;){size_t n=bytes-at;if(n>4096)n=4096;
        int sent=usb_serial_jtag_write_bytes((const uint8_t *)data+at,n,portMAX_DELAY);if(sent>0)at+=sent;}
    putchar('\n');
}
void app_main(void)
{
    usb_serial_jtag_driver_config_t usb=USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();usb.rx_buffer_size=usb.tx_buffer_size=8192;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));usb_serial_jtag_vfs_use_driver();
    static FILE output=FDEV_SETUP_STREAM(console_put,NULL,NULL,_FDEV_SETUP_WRITE);stdout=&output;setvbuf(stdin,NULL,_IONBF,0);
    char line[64];
    for(;;){
        if(!fgets(line,sizeof line,stdin)){vTaskDelay(1);continue;}
        if(!strchr(line,'\n')&&strlen(line)==sizeof line-1){int c;do{c=fgetc(stdin);}while(c!='\n'&&c!=EOF);puts("@{\"error\":\"command too long\"}\n@{\"done\":true}");continue;}
        line[strcspn(line,"\r\n")]=0;
        if(!strcmp(line,"info")){
            uint8_t mac[6];ESP_ERROR_CHECK(esp_read_mac(mac,ESP_MAC_WIFI_STA));
            printf("@{\"cmd\":\"info\",\"app\":\"rx_stream\",\"firmware\":\"%s\",\"tx\":false,"
                "\"psram_bytes\":%u,\"cpu_hz\":%u,\"stream_units\":%u,\"nominal_seconds\":%u.%06u,\"engine_limit_cycles\":%llu,"
                "\"input_rate_sps\":16000000,\"output_rate_sps\":4000000,\"format\":\"sc8\","
                "\"ring_bytes\":471040,\"slots\":64,\"sample_bytes_max\":14288,\"whole_record_allocated\":false,"
                "\"collector_priority\":0,\"sink_priority\":5,\"IWDT_ms\":800,\"TWDT_s\":5,"
                "\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\"}\n",esp_app_get_description()->version,
                (unsigned)esp_psram_get_size(),(unsigned)esp_clk_cpu_freq(),(unsigned)STREAM_UNITS,
                (unsigned)(STREAM_NOMINAL_US/1000000U),(unsigned)(STREAM_NOMINAL_US%1000000U),
                (unsigned long long)STREAM_LIMIT_CYCLES,mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
        }else if(!strcmp(line,"leafcheck")){
            valid=false;rx_stream_release(&last);memset(&last,0,sizeof last);
            esp_err_t e=rx_leafcheck(&last.capture);
            printf("@{\"cmd\":\"leafcheck\",\"complete\":%s,\"status\":\"%s\",\"stage_guards\":%s,"
                   "\"stage_freed\":%s,\"radio_initialized\":false,\"MMIO_writes\":false}\n",
                   e==ESP_OK?"true":"false",esp_err_to_name(e),last.capture.stage_guards_ok?"true":"false",
                   last.capture.stage_freed?"true":"false");leaf_result();
        }else if(!strcmp(line,"stream")){
            valid=false;rx_stream_release(&last);memset(&last,0,sizeof last);
            for(unsigned n=0;n<4;++n)snapshot.before[n]=snapshot.after[n]=0x6d6d6d6dU;
            (void)rx_stream(2420,24,snapshot.raw,&last);valid=qualified();report();
        }else if(!strcmp(line,"snapshot")&&last.capture.snapshot_pairs==512&&snapshot_guards())
            download(snapshot.raw,2048,last.capture.snapshot_crc32,"snapshot");
        else if(!strcmp(line,"matched")&&last.matched_prefix&&last.matched_prefix_guards&&last.matched_prefix_pairs)
            download(last.matched_prefix,4U*last.matched_prefix_pairs,last.matched_prefix_crc,"matched");
        else if((!strcmp(line,"chunk 0")||!strcmp(line,"chunk 1"))&&last.saved&&last.saved_guards){
            unsigned n=line[6]-'0';if(last.sample_bytes[n])download(last.saved+n*CH_SLOT_BYTES,last.sample_bytes[n],last.sample_crc[n],n?"chunk1":"chunk0");
            else puts("@{\"error\":\"chunk unavailable\"}");
        }else puts("@{\"error\":\"use info, leafcheck, stream, snapshot, chunk 0|1, matched\"}");
        puts("@{\"done\":true}");
    }
}
