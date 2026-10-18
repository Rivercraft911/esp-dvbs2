// Frame ring between the encoder and the bank fill (ring.h). Portable C11: tests/test_ring.py
// builds it for the host and runs it against a model of the bank schedule.

#include "ring.h"

#include <string.h>

#include "esp_attr.h"
#include "vcm_cache.h"

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

void ring_publish_frame(ring_t *r, uint32_t e, uint32_t symbols, uint32_t *lengths)
{
    lengths[e % r->nslots] = symbols;
    ring_publish(r, e);
}

static inline uint32_t frame_length(const ring_t *r, uint32_t e, const uint32_t *lengths)
{
    const uint32_t n = lengths[e % r->nslots];
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

#define RING_READ ring_read
#define RING_QUADS (r->packed == 4)
#define RING_VARIABLE 0
#define RING_TEXT IRAM_ATTR
#define RING_LENGTHS
#define RING_ARGUMENT
#include "ring_read.inc"
#undef RING_READ
#undef RING_QUADS
#undef RING_VARIABLE
#undef RING_TEXT
#undef RING_LENGTHS
#undef RING_ARGUMENT
#define RING_READ ring_read_vcm
#define RING_QUADS 0
#define RING_VARIABLE 1
#define RING_TEXT VCM_TEXT
#define RING_LENGTHS , const uint32_t *lengths
#define RING_ARGUMENT , lengths
#include "ring_read.inc"
#undef RING_READ
#undef RING_QUADS
#undef RING_VARIABLE
#undef RING_TEXT
#undef RING_LENGTHS
#undef RING_ARGUMENT

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

#define RING_TEXT IRAM_ATTR
#define RING_LENGTHS
#define RING_ARGUMENT
#define RING_QUADS (r->packed == 4)
#define RING_WINDOW ring_window
#define RING_READ ring_read
#include "ring_window.inc"
#undef RING_QUADS
#undef RING_WINDOW
#undef RING_TEXT
#undef RING_LENGTHS
#undef RING_ARGUMENT
#undef RING_READ
#define RING_TEXT VCM_TEXT
#define RING_LENGTHS , const uint32_t *lengths
#define RING_ARGUMENT , lengths
#define RING_QUADS 0
#define RING_WINDOW ring_window_vcm
#define RING_READ ring_read_vcm
#include "ring_window.inc"
#undef RING_QUADS
#undef RING_WINDOW
#undef RING_TEXT
#undef RING_LENGTHS
#undef RING_ARGUMENT
#undef RING_READ
