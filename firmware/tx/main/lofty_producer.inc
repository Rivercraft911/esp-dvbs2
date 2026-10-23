// The 8PSK 26m667 producer: descriptor pages, EOF publication and admission.
static uint32_t lofty_phases(uint32_t w)
{
    return (((w>>4)&511)<<15) | ((((w>>13)&2047)/3)<<6) | ((w>>24)&63);
}

// Once two pages exist, bulk mapping is finished. Later publications contain
// only descriptor preparation. Use twice their measured worst active gap;
// full-queue sleep is covered separately by one complete bank of wake latency.
// Use the renderer's conservative issue bound: 23 cycles/block.
// For missing page k: wake + k*W <= (P-reserve+k-1)*page_cycles.
static unsigned lofty_admission(uint32_t gap, unsigned wake)
{
    const unsigned period = 23*LOFTY_BLOCKS;
    const unsigned available = (LOFTY_FRAME_PAGES-1-LOFTY_RESERVE_PAGES)*period-wake;
    if (gap > available/2) return LOFTY_FRAME_PAGES;
    unsigned w = 2*gap;
    unsigned p = LOFTY_RESERVE_PAGES+(wake+w+period-1)/period;
    if (p < 2) p = 2;
    if (w > period && p < LOFTY_FRAME_PAGES-available/w)
        p = LOFTY_FRAME_PAGES-available/w;
    return p < LOFTY_FRAME_PAGES ? p : LOFTY_FRAME_PAGES;
}

static __attribute__((noinline)) void lofty_publish(unsigned end)
{
    lofty_page *p = lofty_page_at(dvb.put_index);
    p->words[LOFTY_BLOCKS] = 0;
    p->words[LOFTY_PAGE_META] = end | (lofty_mux.skip<<16);
    lofty_mux.samples += end-lofty_mux.skip;
    p->words[LOFTY_PAGE_META+1] = lofty_mux.samples;
    if (end == MACHINE_PAGE_SAMPLES)
        p->words[LOFTY_PAGE_META+2] = ((lofty_phases(p->words[0])&4095)<<12) |
                                    (lofty_phases(p->words[1])&4095);
    lofty_mux.skip = 0;
    const uint32_t tail = esp_cpu_get_cycle_count();
    dvb.page_blocks = 0;
    if (++dvb.put_index == dvb.queue_pages) dvb.put_index = 0;
    __atomic_store_n(&dvb.produced,dvb.produced+1,__ATOMIC_RELEASE);
    const uint32_t published = esp_cpu_get_cycle_count();
    if (dvb.prepare_pages) {
        uint32_t gap = published-dvb.page_clock-(uint32_t)(dvb.queue_wait_cycles-dvb.page_wait);
        if (gap > dvb.page_gap_max) dvb.page_gap_max = gap;
        if (dvb.mapper.at > 1024 && (gap > lofty_mux.page_max || lofty_mux.page_max == UINT32_MAX)) {
            if (lofty_mux.admission_gap && lofty_mux.admission_gap != UINT32_MAX &&
                gap > lofty_mux.admission_gap && gap-lofty_mux.admission_gap > lofty_mux.admission_gap &&
                __atomic_load_n(&dvb.consumed,__ATOMIC_ACQUIRE)) ++lofty_mux.rate_misses;
            lofty_mux.page_max = gap;
        }
    }
    dvb.page_clock = published;
    dvb.page_wait = dvb.queue_wait_cycles;
    uint32_t spent = esp_cpu_get_cycle_count()-tail;
    if (spent > dvb.prepare_tail_max) dvb.prepare_tail_max = spent;
    ++dvb.prepare_pages;
}

static void lofty_calibrate(void)
{
    if (lofty_mux.page_max == lofty_mux.admission_gap) return;
    const uint32_t before = esp_cpu_get_cycle_count();
    const unsigned gap = lofty_mux.page_max;
    const unsigned pages = lofty_admission(gap,0) | (lofty_admission(gap,LOFTY_BANK_CYCLES)<<8);
    __atomic_store_n(&lofty_mux.admission_pages,pages,__ATOMIC_RELEASE);
    lofty_mux.admission_gap = gap;
    const uint32_t spent = esp_cpu_get_cycle_count()-before;
    if (spent > lofty_mux.admission_max) lofty_mux.admission_max = spent;
}

static __attribute__((noinline)) int lofty_finish(void)
{
    // Publish EOF now, without waiting for the next frame's FEC. The next
    // frame repeats and skips a two-symbol prefix when necessary.
    if (dvb.mapper.pending) {
        while (!enc.stop && dvb.produced-__atomic_load_n(&dvb.consumed,__ATOMIC_ACQUIRE) == dvb.queue_pages)
            moon_wait();
        if (enc.stop) return -1;
        uint16_t word[16] __attribute__((aligned(16))) = {(dvb.mapper.history&63)<<6};
        lofty_prepare(lofty_page_at(dvb.put_index)->words+dvb.page_blocks,word,1,dvb.last_word);
        ++dvb.page_blocks;
    }
    if (dvb.page_blocks) {
        // The consumer needs these four phases only when a source changes.
        // Publish them with EOF instead of decoding a descriptor during fill.
        lofty_page_at(dvb.put_index)->words[LOFTY_PAGE_META+2] = dvb.mapper.history&4095;
        lofty_publish(12*dvb.page_blocks-3*dvb.mapper.pending);
    }
    __atomic_store_n(&lofty_mux.published,lofty_mux.published+1,__ATOMIC_RELEASE);
    // Do not time threshold calculation as the next descriptor page's work.
    // Its cold flash path otherwise raises its own rate bound permanently.
    lofty_calibrate();
    return 0;
}

// Map this frame's FEC bits for the page loop: the first 8PSK chunks or the QPSK sign planes.
static inline int moon_start(void)
{
    if (dvb.lofty) lofty_mux.skip = 3*dvb.mapper.pending;
    return dvb.lofty ? dvbs2_fast_lofty_start(enc.fast,enc.ws,&dvb.mapper) :
                       dvbs2_fast_axis_start(enc.fast,enc.ws,&dvb.mapper);
}

// The page loop: read framed words (8PSK frames the rest on its second read), prepare, publish.
static IRAM_ATTR __attribute__((noinline)) int moon_map(bool warmup)
{
    while (!enc.stop && dvb.mapper.at < dvb.ring.frame_symbols) {
        while (!warmup && !enc.stop && dvb.produced-__atomic_load_n(&dvb.consumed,__ATOMIC_ACQUIRE) == dvb.queue_pages)
            moon_wait();
        if (enc.stop) return -1;
        machine_page *page = dvb.pages ? dvb.pages+dvb.put_index : NULL;
        lofty_page *lp = dvb.lofty ? lofty_page_at(dvb.put_index) : NULL;
        const TickType_t tick = xTaskGetTickCount();
        const uint32_t before = esp_cpu_get_cycle_count();
        const unsigned room = MACHINE_PAGE_BLOCKS-dvb.page_blocks;
        unsigned n = room;
        uint32_t read;
        if (lp) {
            const uint16_t *words = dvbs2_fast_lofty_read(enc.fast,&dvb.mapper,&n);
            read = esp_cpu_get_cycle_count();
            if (n) dvb.last_word = lofty_prepare(lp->words+dvb.page_blocks,words,n,dvb.last_word);
        } else {
            n = dvbs2_fast_axis_read(enc.fast,&dvb.mapper,page->axes+dvb.page_words,
                                     MACHINE_PAGE_AXIS_WORDS-dvb.page_words);
            read = esp_cpu_get_cycle_count();
        }
        // Warm the same mapper/prepare kernels without publishing or advancing
        // the stream. One owned queue page is enough for every discarded chunk.
        if (warmup) { dvb.page_blocks = dvb.page_words = 0; continue; }
        const uint32_t cycles = esp_cpu_get_cycle_count()-before;
        dvb.read_cycles += read-before;
        if (cycles > dvb.prepare_max) {
            dvb.prepare_max = cycles;
            dvb.prepare_ticks = xTaskGetTickCount()-tick;
        }
        dvb.prepare_cycles += cycles;
        if (lp) dvb.page_blocks += n;
        else dvb.page_words += n;
        if (lp ? dvb.page_blocks == LOFTY_BLOCKS : dvb.page_words == MACHINE_PAGE_AXIS_WORDS) {
            if (lp) { lofty_publish(MACHINE_PAGE_SAMPLES); continue; }
            const uint32_t tail = esp_cpu_get_cycle_count();
            if (dvb.prepare_pages) {
                const uint32_t gap = tail-dvb.page_clock-(uint32_t)(dvb.queue_wait_cycles-dvb.page_wait);
                if (gap > dvb.page_gap_max) dvb.page_gap_max = gap;
            }
            dvb.page_clock = tail;
            dvb.page_wait = dvb.queue_wait_cycles;
            dvb.page_words = 0;
            if (++dvb.put_index == dvb.queue_pages) dvb.put_index = 0;
            __atomic_store_n(&dvb.produced,dvb.produced+1,__ATOMIC_RELEASE);
            const uint32_t spent = esp_cpu_get_cycle_count()-tail;
            if (spent > dvb.prepare_tail_max) dvb.prepare_tail_max = spent;
            ++dvb.prepare_pages;
        }
    }
    if (dvb.lofty && !warmup && !enc.stop) return lofty_finish();
    return enc.stop ? -1 : 0;
}
