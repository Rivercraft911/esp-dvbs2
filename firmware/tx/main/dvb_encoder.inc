static void ENCODER_TASK(void *arg)
{
    encoder_t *e = arg;
#if ENCODER_VCM
    size_t bytes = (e->mode->kbch - 80) / 8;
#else
    const size_t bytes = (e->mode->kbch - 80) / 8;
#endif
    uint8_t *const payload = e->payload;
    uint32_t x = SEED;
    const unsigned input = spi_input_type();
    dvbs2_ts_state ts = {0};
#if ENCODER_VCM
    spi_input_control(vcm_control);
#endif
    if (spi_input_start() != ESP_OK) { e->failed = true; goto done; }
    e->input_started = true;
    // The radio worker fixes an automatic queue (radio_s3_stream_ready). Sleep here,
    // with no published pages or pending map, until its immutable layout exists.
    if (dvb.lofty && lofty_mux.queue_auto) {
        while (!e->stop && !__atomic_load_n(&lofty_mux.queue_ready,__ATOMIC_ACQUIRE))
            ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        if (e->stop) goto done;
    }
    if (dvb.moon && !input) {
        // Keep cold instruction/data misses outside playback, without another
        // frame buffer. Reset seed and all framing history before publication.
        const uint32_t before = esp_cpu_get_cycle_count();
        for (unsigned n = 0; n < 3; ++n) {
            uint32_t seed = SEED;
            synthetic_payload(e,bytes,&seed);
            if (dvbs2_fast_bb_gcs(e->fast,e->ws,payload,bytes*8)) { e->failed = true; goto done; }
            dvbs2_fast_bch(e->fast,e->ws);
            dvbs2_fast_ldpc(e->fast,e->ws);
            if (moon_start() || moon_map(true)) { e->failed = true; goto done; }
            ++e->warmup_frames;
        }
        e->warmup_cycles = esp_cpu_get_cycle_count()-before;
        dvb.mapper = (dvbs2_sign_state){0};
        dvb.last_word = dvb.page_blocks = 0;
    }
    for (uint32_t f = 0; !e->stop; ++f) {
        __atomic_store_n(&dvb.producer_phase,QUEUE_FEC,__ATOMIC_RELEASE);
        uint8_t *out = NULL;
        while (!dvb.moon && !(out = ring_slot(&dvb.ring, f))) {
            if (e->stop) {
                goto done;
            }
            e->slot_waits++;
            ulTaskNotifyTake(pdTRUE, e->packed ? portMAX_DELAY : 1);
            if (e->failed) goto done;
        }
        encoder_mark_t before;
        encoder_mark_at(&before);
        const uint32_t start = before.clock.cycles, start_wait = before.wait;
        uint32_t input_cycles = 0, mode_cycles = 0;
        if (ENCODER_VCM) {
            const uint32_t t0=esp_cpu_get_cycle_count();
            if (vcm_boundary(e,f)) { e->failed=true; goto done; }
            mode_cycles=esp_cpu_get_cycle_count()-t0;
            vcm_enc.boundary_cycles+=mode_cycles;
            if (mode_cycles > vcm_enc.boundary_max) vcm_enc.boundary_max=mode_cycles;
        }
    #if ENCODER_VCM
        bytes=(e->mode->kbch-80)/8;
#endif
        if (input) {
            size_t used = 0;
            while (used < bytes && !e->stop) {
                const uint32_t t0 = esp_cpu_get_cycle_count();
                const size_t got = spi_input_read(payload + used, bytes - used);
                input_cycles += esp_cpu_get_cycle_count() - t0;
                used += got;
                e->input_pending = used;
                if (used < bytes) {
                    if (got) e->input_partial++;
                    else {
                        e->input_waits++;
                        ulTaskNotifyTake(pdTRUE, 1);
                    }
                }
            }
            if (e->stop) goto done;
            if (input_cycles > e->input_frame_cycles_max) e->input_frame_cycles_max = input_cycles;
        } else synthetic_payload(e,bytes,&x);
        uint32_t kernel_start, kernel_wait;
        if (dvb.profile) {
            encoder_mark_t after;
            encoder_mark_at(&after);
            if (input) before.clock.cycles = after.clock.cycles-input_cycles;
            stream_record_at(&e->stage[0],&before.clock,&after.clock);
            // RX sleeps are excluded from input_cycles.
            encoder_mark_copy(&before,&after);
            kernel_start = before.clock.cycles, kernel_wait = before.wait;
        } else {
            kernel_start = esp_cpu_get_cycle_count();
            kernel_wait = (uint32_t)dvb.queue_wait_cycles;
        }
        int rc;
        if (dvb.profile || dvb.moon) {
            rc = input == PV_TS ? dvbs2_fast_bb_ts(e->fast, e->ws, &ts, payload, bytes) :
                                  dvbs2_fast_bb_gcs(e->fast, e->ws, payload, bytes * 8);
            encoder_record(&e->stage[1],&before);
            if (!rc) {
                dvbs2_fast_bch(e->fast, e->ws);
                encoder_record(&e->stage[2],&before);
                dvbs2_fast_ldpc(e->fast, e->ws);
                encoder_record(&e->stage[3],&before);
                if (dvb.moon) {
                    __atomic_store_n(&dvb.producer_phase,QUEUE_MAP,__ATOMIC_RELEASE);
                    rc = moon_start();
                    encoder_record(&e->stage[4],&before);
                    if (!rc) rc = moon_map(false);
                    encoder_record(&e->stage[6],&before);
                } else if (e->packed) {
                    dvbs2_fast_map_packed(e->fast, e->ws, out);
                } else {
                    dvbs2_fast_map(e->fast, e->ws, out);
                }
                encoder_record(&e->stage[4],&before);
                __atomic_store_n(&dvb.producer_phase,QUEUE_PL,__ATOMIC_RELEASE);
                if (!dvb.moon) {
                    if (e->packed) dvbs2_fast_pl_packed(e->fast,out);
                    else dvbs2_fast_pl(e->fast,out);
                }
                encoder_record(&e->stage[5],&before);
            }
        } else if (e->packed) {
            rc = dvbs2_encode_frame_packed(e->fast, e->ws, input == PV_TS ? &ts : NULL, payload, bytes * 8, out);
        } else {
            rc = dvbs2_encode_frame(e->fast, e->ws, input == PV_TS ? &ts : NULL, payload, bytes * 8, out);
        }
        if (rc) {
            if (e->stop) goto done;
            e->failed = true;
            goto done;
        }
        const uint32_t end_wait = (uint32_t)dvb.queue_wait_cycles, end = esp_cpu_get_cycle_count();
        // As with waiting for a legacy frame slot, full-queue backpressure is
        // outside encoder work. Interrupts and runnable-task delays stay timed.
        const uint32_t kernel_spent = end-kernel_start-(end_wait-kernel_wait);
        const uint32_t spent = input ? mode_cycles+input_cycles+kernel_spent :
            end-start-(end_wait-start_wait);
        const uint32_t work = kernel_spent;
        const uint32_t frame_work = spent;
        e->frame_work_cycles += frame_work;
        if (f >= (dvb.moon ? 2 : dvb.ring.nslots) && frame_work > e->frame_work_live_max) e->frame_work_live_max = frame_work;
        e->work_cycles += work;
        if (f >= (dvb.moon ? 2 : dvb.ring.nslots) && work > e->work_live_max) e->work_live_max = work;
        e->input_pending = 0;
        if (ENCODER_VCM) {
            vcm_enc.mode_frames[e->mode->modcod-4]++;
            ring_publish_frame(&dvb.ring,f,dvbs2_frame_symbols(e->mode,PILOTS),vcm_enc.lengths);
        } else if (!dvb.moon) ring_publish(&dvb.ring, f);
        e->frames = f + 1;
        e->cycles += spent;
        e->kernel_cycles += kernel_spent;
        e->min_cycles = spent < e->min_cycles ? spent : e->min_cycles;
        e->max_cycles = spent > e->max_cycles ? spent : e->max_cycles;
        if (spent > e->status_max_cycles) e->status_max_cycles = spent;
        if (f >= (dvb.moon ? 2 : dvb.ring.nslots) && spent > e->live_max_cycles) e->live_max_cycles = spent;
        if (f >= (dvb.moon ? 2 : dvb.ring.nslots) && kernel_spent > e->kernel_live_max) e->kernel_live_max = kernel_spent;
        if (e->stall_ms) {
            vTaskDelay(pdMS_TO_TICKS(e->stall_ms));
        }
    }
done:
    spi_input_stop();
    e->stack_min_free = uxTaskGetStackHighWaterMark(NULL);
    xSemaphoreGive(e->exited);
    // Keep the notification handle valid even if encoding failed. The command owns
    // deletion after taking exited; a fill may still notify this suspended task.
    vTaskSuspend(NULL);
}
