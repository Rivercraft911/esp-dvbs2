// Frame ring between the encoder and the bank fill (ring.h). Portable C11: tests/test_ring.py
// builds it for the host and runs it against a model of the bank schedule.

#include "ring.h"

#include <string.h>

#include "esp_attr.h"

void ring_init(ring_t *r, uint8_t *slots, uint32_t nslots, const uint8_t *filler,
               uint32_t frame_symbols)
{
    memset(r, 0, sizeof *r);
    r->nslots = nslots;
    for (uint32_t i = 0; i < nslots; ++i) r->slots[i] = slots + (size_t)i * frame_symbols;
    r->filler = filler;
    r->frame_symbols = r->filler_symbols = frame_symbols;
    r->min_lead = INT64_MAX;
}

void ring_init_slots(ring_t *r, uint8_t *const *slots, uint32_t nslots,
                     const uint8_t *filler, uint32_t frame_symbols)
{
    memset(r, 0, sizeof *r);
    r->nslots = nslots;
    for (uint32_t i = 0; i < nslots; ++i) r->slots[i] = slots[i];
    r->filler = filler;
    r->frame_symbols = r->filler_symbols = frame_symbols;
    r->min_lead = INT64_MAX;
}

uint8_t *ring_slot(ring_t *r, uint32_t e)
{
    // The acquire pairs with the fill's release of `freed`: its reads of the slot are done.
    if (e - __atomic_load_n(&r->freed, __ATOMIC_ACQUIRE) >= r->nslots) {
        return NULL;
    }
    return r->slots[e % r->nslots];
}

void ring_publish(ring_t *r, uint32_t e)
{
    __atomic_store_n(&r->published, e + 1, __ATOMIC_RELEASE);
}

void ring_publish_frame(ring_t *r, uint32_t e, uint32_t symbols)
{
    r->slot_symbols[e % r->nslots] = symbols;
    ring_publish(r, e);
}

static inline uint32_t frame_length(const ring_t *r, uint32_t e)
{
    const uint32_t n = r->slot_symbols[e % r->nslots];
    return n ? n : r->frame_symbols;
}

void ring_filler_length(ring_t *r, uint32_t symbols)
{
    r->filler_symbols = symbols;
}

void ring_cache(ring_t *r, uint8_t *storage, size_t symbols)
{
    r->cache = storage;
    r->cache_symbols = symbols;
}

void ring_packed(ring_t *r)
{
    r->packed = 1;
}

void ring_packed_codes(ring_t *r)
{
    r->packed = 2;
}

void ring_packed_qpsk(ring_t *r)
{
    r->packed = 3;
}

#ifdef __XTENSA__
// shaper_pie.S: 16 packed bytes (any alignment) to 32 phases at a 16-byte aligned dst.
void shaper_unpack_pie(uint8_t *dst, const uint8_t *src, unsigned blocks);
void shaper_unpack_codes_pie(uint8_t *dst, const uint8_t *src, unsigned blocks);
void shaper_unpack_qpsk_pie(uint8_t *dst, const uint8_t *src, unsigned blocks);
#endif

static inline __attribute__((always_inline)) void unpack(uint8_t *dst, const uint8_t *packed,
                                                         size_t at, size_t n, unsigned shift, unsigned drop)
{
    const uint8_t *p = packed + at / 2;
    if (n && (at & 1)) {
        *dst++ = (*p++ >> (4 + drop)) << shift;
        --n;
    }
#ifdef __XTENSA__
    // Two phases per byte up to an aligned dst, then whole vectors. An odd distance would
    // split a byte across the boundary, so that case stays on the scalar loop.
    size_t head = -(uintptr_t)dst & 15;
    if (!(head & 1) && n >= head + 32) {
        for (n -= head; head; head -= 2) {
            const uint8_t b = *p++;
            dst[0] = ((b & 15) >> drop) << shift;
            dst[1] = (b >> (4 + drop)) << shift;
            dst += 2;
        }
        const size_t blocks = n / 32;
        if (drop) shaper_unpack_qpsk_pie(dst, p, (unsigned)blocks);
        else if (shift) shaper_unpack_codes_pie(dst, p, (unsigned)blocks);
        else shaper_unpack_pie(dst, p, (unsigned)blocks);
        dst += 32 * blocks, p += 16 * blocks, n -= 32 * blocks;
    }
#endif
    for (; n >= 2; n -= 2) {
        const uint8_t b = *p++;
        dst[0] = ((b & 15) >> drop) << shift;
        dst[1] = (b >> (4 + drop)) << shift;
        dst += 2;
    }
    if (n) {
        *dst = ((*p & 15) >> drop) << shift;
    }
}

IRAM_ATTR void ring_unpack(uint8_t *dst, const uint8_t *packed, size_t at, size_t n)
{
    unpack(dst, packed, at, n, 0, 0);
}

IRAM_ATTR void ring_unpack_codes(uint8_t *dst, const uint8_t *packed, size_t at, size_t n)
{
    unpack(dst, packed, at, n, 4, 0);
}

IRAM_ATTR void ring_unpack_qpsk(uint8_t *dst, const uint8_t *packed, size_t at, size_t n)
{
    unpack(dst, packed, at, n, 4, 1);
}

// Four two-bit codes per byte, high pair first. Byte/vector copies dominate;
// only the two edges use per-symbol insertion. Source has sixteen guard bytes.
IRAM_ATTR void ring_copy_quads(uint8_t *dst, size_t to, const uint8_t *src, size_t at, size_t n)
{
    while (n && (to & 3)) {
        unsigned shift = 6 - 2*(to & 3), v = (src[at/4] >> (6 - 2*(at & 3))) & 3;
        dst[to/4] = (dst[to/4] & ~(3u << shift)) | (v << shift);
        ++to, ++at, --n;
    }
    uint8_t *d = dst + to/4;
    const uint8_t *p = src + at/4;
    size_t bytes = n/4;
    const unsigned shift = 2*(at & 3);
#ifdef __XTENSA__
    while (bytes && ((uintptr_t)d & 15)) {
        *d++ = shift ? (p[0] << shift) | (p[1] >> (8-shift)) : p[0];
        ++p, --bytes;
    }
    if (bytes >= 16) {
        void ring_copy_quads_pie(uint8_t *, const uint8_t *, unsigned, unsigned);
        const unsigned blocks = bytes/16;
        ring_copy_quads_pie(d, p, blocks, shift);
        d += 16*blocks, p += 16*blocks, bytes -= 16*blocks;
    }
#endif
    while (bytes--) {
        *d++ = shift ? (p[0] << shift) | (p[1] >> (8-shift)) : p[0];
        ++p;
    }
    at += n & ~(size_t)3;
    for (unsigned i = 0; i < (n & 3); ++i) {
        unsigned v = (src[(at+i)/4] >> (6 - 2*((at+i) & 3))) & 3;
        *d = (*d & ~(3u << (6-2*i))) | (v << (6-2*i));
    }
}

// Copy absolute symbols, retaining choices until no future overlap can read them.
static IRAM_ATTR int ring_read(ring_t *r, uint8_t *dst, size_t to, int64_t first, size_t count)
{
    const uint32_t published = __atomic_load_n(&r->published, __ATOMIC_ACQUIRE);
    const int64_t end = first + (int64_t)count;
    if (first < 0) {
        const size_t zeros = end < 0 ? count : (size_t)-first;
        // Before symbol zero, quad code zero is phase 1, as in the legacy shaper.
        memset(dst, 0, r->packed == 4 ? (zeros+3)/4 : zeros);
        if (r->packed == 4) to += zeros;
        else dst += zeros;
        count -= zeros, first = 0;
    }
    if (!count) {
        return 0;
    }
    if ((uint64_t)first < (r->release_position == r->next_position ? r->chosen_end :
                           r->start[r->release_position % RING_CHOICES])) {
        return -1;
    }
    uint32_t p = r->release_position;
    while (p < r->next_position && r->end[p % RING_CHOICES] <= (uint64_t)first) {
        ++p;
    }
    while (r->chosen_end < (uint64_t)end) {
        if (r->next_position - r->release_position >= RING_CHOICES) {
            return -1;
        }
        const unsigned i = r->next_position++ % RING_CHOICES;
        uint32_t c = RING_FILLER;
        if (r->next_frame != published) {
            c = r->next_frame++;
        } else {
            r->underruns++;
        }
        r->choice[i] = c;
        r->start[i] = r->chosen_end;
        r->chosen_end += c == RING_FILLER ? r->filler_symbols : frame_length(r, c);
        r->end[i] = r->chosen_end;
    }
    int64_t lead = (int64_t)r->chosen_end - end;
    for (uint32_t e = r->next_frame; e != published; ++e) lead += frame_length(r, e);
    if (lead < r->min_lead) {
        r->min_lead = lead;
    }
    while (count) {
        const unsigned i = p++ % RING_CHOICES;
        const uint32_t c = r->choice[i];
        const uint8_t *src = c == RING_FILLER ? r->filler :
                            r->slots[c % r->nslots];
        const size_t at = (uint64_t)first - r->start[i];
        const size_t avail = r->end[i] - (uint64_t)first;
        const size_t part = avail < count ? avail : count;
        if (r->packed == 4) {
            ring_copy_quads(dst, to, src, at, part);
        } else if (r->packed == 3) {
            ring_unpack_qpsk(dst, src, at, part);
        } else if (r->packed == 2) {
            ring_unpack_codes(dst, src, at, part);
        } else if (r->packed) {
            ring_unpack(dst, src, at, part);
        } else {
            memcpy(dst, src + at, part);
        }
        if (r->packed == 4) to += part;
        else dst += part;
        count -= part, first += part;
    }
    return 0;
}

static IRAM_ATTR void ring_release(ring_t *r, int64_t oldest)
{
    uint32_t freed = 0;
    while (r->release_position < r->next_position &&
           r->end[r->release_position % RING_CHOICES] <= (uint64_t)oldest) {
        const uint32_t c = r->choice[r->release_position++ % RING_CHOICES];
        if (c != RING_FILLER) {
            freed = c + 1;
        }
    }
    if (freed) {
        __atomic_store_n(&r->freed, freed, __ATOMIC_RELEASE);
    }
}

IRAM_ATTR int ring_window(ring_t *r, uint8_t *dst, size_t room, uint64_t first, size_t count,
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
    if (ring_read(r, r->packed == 4 ? dst : dst + cached, r->packed == 4 ? cached : 0,
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
