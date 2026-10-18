// One overlap/notification algorithm, specialized once at stream admission.
static FILL_TEXT esp_err_t FILL_NAME(uint32_t *dst, uint64_t first, size_t count, void *ctx)
{
    (void)ctx;
    // Keep the bank timer and encoder profiling. Per-phase marks and 64-bit
    // accumulators belong to untimed 8PSK dumps, not its playback deadline.
#if LOFTY_STREAM_FUSED
    const bool phases = false; // fused is timed playback only, never an untimed dump
#else
    const bool phases = dvb.profile && (!FILL_LOFTY || dvb.copy.counters);
#endif
    const uint32_t before = esp_cpu_get_cycle_count();
    const stream_mark_t mark = phases ? stream_mark_for(&dvb.copy) : (stream_mark_t){0};
    const size_t requested = count;
    const size_t cache_words = dvb.cached_words;
    // radio_s3 calls this once to the physical bank end, then for the wrapped
    // prefix. Save only the suffix that survives that second callback.
    // radio_s3_bank() is 0x3fcc0000 or 0x3fcd0000: two 64 KiB aligned banks.
    // Use the physical pointer. Station ID removes samples from `first`, so
    // its modulo is neither the bank offset nor a reliable completion test.
    const size_t offset = ((uintptr_t)dst & 0xffff)/sizeof *dst;
    const bool bank_done = !offset || requested < RADIO_S3_BANK_WORDS-offset;
    const size_t save_words = offset && requested == RADIO_S3_BANK_WORDS - offset ?
        (offset < cache_words ? cache_words - offset : 0) : cache_words;
    // Copy any re-read samples before touching the ring. Fresh reads then move strictly
    // forwards, so a short second callback at the physical output wrap cannot shift hundreds
    // of cached symbols. Never read the other physical DAC bank: it may belong to the radio.
    if (FILL_PACKED && first < dvb.sample_end) {
        const uint64_t back = dvb.sample_end - first;
        if (back > cache_words) return ESP_FAIL;
        size_t at = dvb.sample_at + cache_words - (size_t)back;
        if (at >= cache_words) at -= cache_words;
        const size_t cached = back < count ? (size_t)back : count;
        const size_t head = cached < cache_words - at ? cached : cache_words - at;
        shaper_copy_words(dst, dvb.samples + at, head);
        shaper_copy_words(dst + head, dvb.samples, cached - head);
        dst += cached, first += cached, count -= cached;
    }
    const unsigned sps = dvb.table.n;
    const uint32_t freed = dvb.ring.freed;
    // Packed reads unpack whole vectors when every symbol lands at an address of its own
    // parity (ring.h); the history is even, so shift the window by the first symbol's parity.
    // sps is 4 or 8 there, so that parity is bit log2(sps) of first. Legacy n40 keeps the window.
    uint8_t *win = dvb.win + (!FILL_QUADS && FILL_PACKED && !FILL_MOON && (first & sps));
    uint64_t read_first = first*dvb.den, read_count = count*dvb.den;
    if (!FILL_MOON && count && (FILL_VCM ? ring_window_vcm : ring_window)(&dvb.ring, win, dvb.win_bytes - 1, read_first, read_count, sps,
                    dvb.history, RADIO_S3_BANK_WORDS*dvb.den) < 0) return ESP_FAIL;
    const uint32_t copied = esp_cpu_get_cycle_count() - before;
    const uint64_t w0 = dvb.whole.cycles, p0 = dvb.partial.cycles;
    if (phases) stream_record(&dvb.copy, mark, stream_mark_for(&dvb.copy));
    uint32_t notified = 0, saved = 0;
    if (FILL_MOON) {
        const stream_mark_t whole = phases ? stream_mark_for(&dvb.whole) : (stream_mark_t){0};
        if (count && (FILL_LOFTY ? lofty_shape(dst,first,count) : moon_shape(dst,first,count)) != ESP_OK) return ESP_FAIL;
        if (phases) stream_record(&dvb.whole, whole, stream_mark_for(&dvb.whole));
    } else if (FILL_QUADS) {
        const stream_mark_t whole = phases ? stream_mark_for(&dvb.whole) : (stream_mark_t){0};
        shaper_qpsk_samples(dst, win + 2, first % (4*sps), count, dvb.table.blocks, sps);
        if (phases) stream_record(&dvb.whole, whole, stream_mark_for(&dvb.whole));
    } else (FILL_VCM && enc.vcm_axes ? shaper_axes_samples_profile :
               FILL_PACKED ? shaper_samples_codes_profile : shaper_samples_profile)(dst,
        win + SHAPER_HISTORY, first % sps, count, &dvb.table,
        phases ? &dvb.whole : NULL, phases ? &dvb.partial : NULL);
    if (FILL_PACKED && count) {
        // The completed bank retains exactly cache_words fresh samples in total.
        // Between callbacks the cache may be incomplete; the second is contiguous
        // and cannot re-read any sample after a first callback that shaped words.
        const stream_mark_t save = phases ? stream_mark_for(&dvb.save) : (stream_mark_t){0};
        const size_t keep = count < save_words ? count : save_words;
        const uint32_t *suffix = dst + count - keep;
        if (keep == cache_words) {
            shaper_copy_words(dvb.samples, suffix, keep);
            dvb.sample_at = 0;
        } else {
            const size_t head = keep < cache_words - dvb.sample_at ? keep :
                                cache_words - dvb.sample_at;
            shaper_copy_words(dvb.samples + dvb.sample_at, suffix, head);
            shaper_copy_words(dvb.samples, suffix + head, keep - head);
            dvb.sample_at += keep;
            if (dvb.sample_at >= cache_words) dvb.sample_at -= cache_words;
        }
        dvb.sample_end = first + count;
        if (phases) {
            const stream_mark_t after = stream_mark_for(&dvb.save);
            stream_record(&dvb.save, save, after);
            saved = after.cycles - save.cycles;
        }
    }
    // The wrapped prefix still needs the shaper SRAM. Hold a released slot's
    // notification until both callbacks finish; preserve it across callbacks.
    if (dvb.ring.freed != freed && FILL_PACKED && !FILL_MOON) dvb.wake_pending = true;
    if (dvb.wake_pending && bank_done) {
        const stream_mark_t wake = phases ? stream_mark_for(&dvb.notify) : (stream_mark_t){0};
        if (FILL_MOON) moon_wake();
        else xTaskNotifyGive(enc.task);
        dvb.wake_pending = false;
        if (phases) {
            const stream_mark_t after = stream_mark_for(&dvb.notify);
            stream_record(&dvb.notify, wake, after);
            notified = after.cycles - wake.cycles;
        }
    }
    if (FILL_MOON && bank_done) {
        if (dvb.wake_bank > dvb.wake_max) dvb.wake_max = dvb.wake_bank;
        dvb.wake_bank = 0;
    }
    const uint32_t cycles = esp_cpu_get_cycle_count() - before;
    if (FILL_MOON && !FILL_LOFTY) {
        dvb.bank_cycles += cycles;
        dvb.bank_whole += dvb.whole.cycles-w0;
        ++dvb.bank_calls;
        if (bank_done) {
            if (dvb.bank_cycles > dvb.max_bank_cycles) {
                dvb.max_bank_cycles = dvb.bank_cycles;
                dvb.max_bank_whole = dvb.bank_whole;
                dvb.max_bank_calls = dvb.bank_calls;
            }
            dvb.bank_cycles = dvb.bank_whole = dvb.bank_calls = 0;
        }
    }
    dvb.fill_words += requested;
    dvb.shaped_words += count;
    dvb.reused_words += requested - count;
    dvb.fill_cycles += cycles;
    dvb.fill_calls++;
    if (cycles > dvb.max_fill_cycles) {
        dvb.max_fill_cycles = cycles;
        dvb.worst_copy = copied;
        dvb.worst_whole = dvb.whole.cycles - w0;
        dvb.worst_partial = dvb.partial.cycles - p0;
        dvb.worst_save = saved;
        dvb.worst_notify = notified;
    }
    return ESP_OK;
}
