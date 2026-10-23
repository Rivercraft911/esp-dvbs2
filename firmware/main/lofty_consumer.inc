// The 8PSK 26m667 bank mux: data pages, dummy frames and the splices between them.
static LOFTY_TEXT void lofty_splice(uint32_t raw, unsigned skip)
{
    unsigned bits = 12-skip; // skip is 0 or 6 samples, also 0 or 6 phase bits
    uint32_t h = ((uint32_t)lofty_mux.history<<bits) | ((raw>>12)&((1u<<bits)-1));
    lofty_mux.head[0] = lofty_descriptor(h);
    lofty_mux.head[1] = lofty_descriptor((h<<12) | (raw&4095));
    lofty_mux.head_at = skip;
    lofty_mux.head_left = 12+skip;
}

// Complete frames always qualify. Streaming starts beyond the bulk-map page,
// with the fastest possible page consumption, bank wakes and nine reserve pages.
static inline __attribute__((always_inline)) void lofty_boundary(uint32_t ready)
{
    bool odd = (lofty_mux.startup+lofty_mux.steady)&1;
    unsigned queued = __atomic_load_n(&dvb.produced,__ATOMIC_ACQUIRE)-dvb.consumed;
    bool complete = (int32_t)(ready-lofty_mux.frames) > 0;
    // The producer owns phase until it has actually resumed. waiting alone
    // is insufficient: moon_wake clears it before the task gets CPU time.
    unsigned phase = __atomic_load_n(&dvb.producer_phase,__ATOMIC_ACQUIRE);
    unsigned asleep = phase != QUEUE_MAP;
    unsigned threshold = (__atomic_load_n(&lofty_mux.admission_pages,__ATOMIC_ACQUIRE)>>(8*asleep))&255;
    lofty_mux.filler = odd || (!complete &&
        queued < threshold);
    if (!lofty_mux.filler && !complete) ++lofty_mux.streaming;
    lofty_mux.at = 0;
    lofty_mux.length = lofty_mux.filler ? 3*DVBS2_DUMMY_SYMBOLS : 3*LOFTY_FRAME_SYMBOLS;
    if (lofty_mux.filler) {
        // A partial next frame may have been left asleep at the previous
        // bank's full-queue wait. Dummy playback releases no data pages.
        if (!odd) { moon_wake(); dvb.wake_pending = false; }
        if (lofty_mux.frames < LOFTY_STARTUP_FRAMES) {
            if (!lofty_mux.startup) lofty_mux.first_dummy_frame = lofty_mux.frames;
            ++lofty_mux.startup;
        }
        else ++lofty_mux.steady;
        // Each cached pair has both possible data-boundary prefixes. The
        // second dummy continues the first one's four-symbol groups.
        unsigned prefix = 2*(lofty_mux.frames&1);
        if (!odd) lofty_splice(lofty_mux.raw[prefix != 0],3*prefix);
        lofty_mux.dummy_at = 3*prefix+(odd ? 3*DVBS2_DUMMY_SYMBOLS : 0);
        lofty_mux.splice = true;
    } else if (lofty_mux.splice) {
        unsigned meta = lofty_page_at(dvb.get_index)->words[LOFTY_PAGE_META];
        lofty_page *p = lofty_page_at(dvb.get_index);
        lofty_splice(p->words[LOFTY_PAGE_META+2],meta>>16);
        lofty_mux.splice = false;
    }
}

// Dummy rendering and source changes share the IRAM vector renderer.
static LOFTY_TEXT unsigned lofty_short(uint32_t *dst, unsigned take)
{
    const uint32_t *src;
    unsigned at;
    if (lofty_mux.head_left) {
        if (take > lofty_mux.head_left) take = lofty_mux.head_left;
        src = lofty_mux.head; at = lofty_mux.head_at;
        lofty_mux.head_at += take; lofty_mux.head_left -= take;
    } else {
        unsigned prefix = 2*(lofty_mux.frames&1);
        src = lofty_mux.dummy+(prefix ? LOFTY_DUMMY_STRIDE : 0);
        at = lofty_mux.dummy_at+lofty_mux.at;
    }
    lofty_samples(dst,src+at/12,at%12,take,shaper_store);
    return take;
}

static __attribute__((noinline)) esp_err_t lofty_fault(uint32_t put, uint32_t get)
{
    ++dvb.queue_empty;
    dvb.queue_fault.put = put; dvb.queue_fault.get = get;
    dvb.queue_fault.phase = __atomic_load_n(&dvb.producer_phase,__ATOMIC_ACQUIRE);
    dvb.queue_fault.wait_put = __atomic_load_n(&dvb.wait_put,__ATOMIC_RELAXED);
    dvb.queue_fault.wait_get = __atomic_load_n(&dvb.wait_get,__ATOMIC_RELAXED);
    return ESP_FAIL;
}

static LOFTY_TEXT esp_err_t lofty_shape(uint32_t *dst, uint64_t first, size_t count)
{
    if (first != lofty_mux.output) return ESP_FAIL;
    const unsigned words = count;
    const uint32_t before = esp_cpu_get_cycle_count();
    uint32_t low = UINT32_MAX;
    while (count) {
        if (!lofty_mux.length) lofty_boundary(__atomic_load_n(&lofty_mux.published,__ATOMIC_ACQUIRE));
        unsigned left = lofty_mux.length-lofty_mux.at;
        unsigned take = count < left ? count : left;
        if (lofty_mux.filler) take = lofty_short(dst,take);
        else {
            uint32_t get = dvb.consumed;
            uint32_t put = __atomic_load_n(&dvb.produced,__ATOMIC_ACQUIRE);
            if (put == get) return lofty_fault(put,get);
            if (put-get < dvb.queue_low) dvb.queue_low = put-get;
            lofty_page *p = lofty_page_at(dvb.get_index);
            unsigned meta = p->words[LOFTY_PAGE_META], end = meta&65535;
            if (!dvb.page_sample) dvb.page_sample = meta>>16;
            if (take > end-dvb.page_sample) take = end-dvb.page_sample;
            unsigned last = dvb.get_index+put-get-1;
            if (last >= dvb.queue_pages) last -= dvb.queue_pages;
            uint32_t end_sample = lofty_page_at(last)->words[LOFTY_PAGE_META+1];
            uint32_t lead = end_sample-(uint32_t)dvb.queue_sample;
            if (!lofty_mux.head_left && !dvb.page_sample && take == MACHINE_PAGE_SAMPLES) {
                // Reuse vector setup across adjacent complete pages, retaining
                // the reserve. Never cross EOF or the physical queue wrap.
                unsigned pages = (count < left ? count : left)/MACHINE_PAGE_SAMPLES;
                unsigned room = dvb.queue_pages-dvb.get_index;
                if (pages > room) pages = room;
                unsigned available = put-get > LOFTY_RESERVE_PAGES ? put-get-LOFTY_RESERVE_PAGES : 1;
                if (pages > available) pages = available;
                take = pages*MACHINE_PAGE_SAMPLES;
                // Publication during the batch can only increase this bound.
                lead -= (pages-1)*MACHINE_PAGE_SAMPLES;
                lofty_render_pages(dst,p->words,pages,shaper_store,&dvb.consumed);
                dvb.get_index += pages;
                if (dvb.get_index == dvb.queue_pages) dvb.get_index = 0;
            } else {
                if (lofty_mux.head_left) take = lofty_short(dst,take);
                else lofty_samples(dst,p->words+dvb.page_sample/12,dvb.page_sample%12,take,shaper_store);
                if (take == left) lofty_mux.history = p->words[LOFTY_PAGE_META+2];
                dvb.page_sample += take;
                if (dvb.page_sample == end) {
                    dvb.page_sample = 0;
                    if (++dvb.get_index == dvb.queue_pages) dvb.get_index = 0;
                    __atomic_store_n(&dvb.consumed,get+1,__ATOMIC_RELEASE);
                }
            }
            if (lead < low) low = lead;
            dvb.queue_sample += take;
            if (!dvb.page_sample) {
                if (put-dvb.consumed <= dvb.queue_pages-dvb.wake_pages) {
                    dvb.wake_pending = true;
                }
            }
        }
        dst += take; count -= take;
        lofty_mux.output += take; lofty_mux.at += take;
        if (lofty_mux.at == lofty_mux.length) {
            if (lofty_mux.filler) lofty_mux.history = lofty_mux.dummy_history;
            else {
                ++lofty_mux.frames;
            }
            lofty_mux.length = 0;
        }
    }
    // Preserve page-entry lead in samples. Conversion belongs in the report.
    // The third head word is prefetched but never used by the renderer.
    if (low < lofty_mux.head[2]) lofty_mux.head[2] = low;
    if (dvb.fill_calls < 2) {
        lofty_mux.handoff[dvb.fill_calls].words = words;
        lofty_mux.handoff[dvb.fill_calls].cycles = esp_cpu_get_cycle_count()-before;
    }
    return ESP_OK;
}
