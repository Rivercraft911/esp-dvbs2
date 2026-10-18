IRAM_ATTR int RING_WINDOW(ring_t *r, uint8_t *dst, size_t room, uint64_t first, size_t count,
                          unsigned sps, unsigned history, unsigned bank_words)
{
    const uint64_t s0 = r->packed == 4 ? first / sps & ~(uint64_t)3 : first / sps;
    const size_t nsym = (size_t)((first + count + sps - 1) / sps - s0);
    const int64_t begin = (int64_t)s0 - history, end = (int64_t)s0 + nsym;
    const size_t need = r->packed == 4 ? (history + nsym + 3)/4 : history + nsym;
    if (need > room) {
        return -1;
    }
    size_t cached = 0;
    if (r->cache && r->cache_first != r->cache_end && begin < r->cache_end) {
        if (begin < r->cache_first) return -1;
        cached = (size_t)((end < r->cache_end ? end : r->cache_end) - begin);
        if (r->packed == 4) ring_copy_quads(dst, 0, r->cache, begin - r->cache_first, cached);
        else memcpy(dst, r->cache + (begin - r->cache_first), cached);
    }
    if (RING_READ(r, r->packed == 4 ? dst : dst + cached, r->packed == 4 ? cached : 0,
                  begin + cached, history + nsym - cached)) return -1;
    if (r->cache) {
        if (end > r->cache_end) {
            int64_t keep = end - (int64_t)r->cache_symbols;
            if (r->packed == 4) {
                // Retain up to three full blocks plus the current partial block. A
                // later fill may start earlier within this same four-symbol block.
                keep = (end - (r->cache_symbols > 16 ? 24 : 12)) & ~(int64_t)3;
                if (keep < begin) keep = begin;
                ring_copy_quads(r->cache, 0, dst, keep - begin, end - keep);
            } else if (keep < begin) {
                // A short second piece at the physical bank wrap must retain the
                // preceding piece's overlap too. Do not replace it with only this piece.
                if (keep < r->cache_first || begin > r->cache_end) keep = begin;
                const size_t prefix = begin - keep;
                if (prefix) memmove(r->cache, r->cache + (keep - r->cache_first), prefix);
                memcpy(r->cache + prefix, dst, end - begin);
            } else {
                memcpy(r->cache, dst + (keep - begin), end - keep);
            }
            r->cache_first = keep;
            r->cache_end = end;
        }
        if (r->cache_first > 0) ring_release(r, r->cache_first);
        return (int)nsym;
    }
    const uint64_t earliest = first + count > bank_words ? first + count - bank_words : 0;
    const int64_t oldest = (int64_t)(earliest / sps) - history;
    if (oldest > 0) {
        ring_release(r, oldest);
    }
    return (int)nsym;
}
