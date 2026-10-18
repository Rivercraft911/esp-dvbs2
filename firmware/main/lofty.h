#pragma once
#include <stddef.h>
#include <stdint.h>

// Five-symbol .35 pulse, three samples/symbol, four symbols/block.
#define LOFTY_TABLE_WORDS 9216
#define LOFTY_BLOCKS 128
#define LOFTY_STORED 38
#define LOFTY_PAGES 50
#define LOFTY_WAKE 2
#define LOFTY_PAGE_META (LOFTY_BLOCKS+1) // after the renderer's prefetch guard
#define LOFTY_FRAME_SYMBOLS 22194 // normal 8PSK frame, pilots on
#define LOFTY_FRAME_PAGES 44           // ceil(22194/512), including partial EOF
typedef struct {
    uint32_t words[LOFTY_BLOCKS+4]; // guard, extent/skip, cumulative end, raw head/EOF history
} __attribute__((aligned(16))) lofty_page;
// Four phases -> three scaled offsets. Previous is the preceding four phases.
// Source has 16 readable guard bytes. Arbitrary destination word alignment.
uint32_t lofty_prepare(uint32_t *, const uint16_t *, unsigned, uint32_t previous);
uint32_t lofty_prepare_pie(uint32_t *, const uint16_t *, unsigned vectors, uint32_t);
int lofty_build(uint32_t *, unsigned peak);
void lofty_render_pie(uint32_t *, const uint32_t *, unsigned, const uint32_t *);
// Consecutive full queue pages. Release each page after its last descriptor.
void lofty_render_pages_pie(uint32_t *, const uint32_t *, unsigned, const uint32_t *, uint32_t *);
#ifdef __XTENSA__
static inline __attribute__((always_inline)) void lofty_render(uint32_t *dst, const uint32_t *src,
                                                             unsigned n, const uint32_t *table)
{
    lofty_render_pie(dst,src,n,table);
}
static inline __attribute__((always_inline)) void lofty_render_pages(uint32_t *dst, const uint32_t *src,
                    unsigned pages, const uint32_t *table, uint32_t *consumed)
{
    lofty_render_pages_pie(dst,src,pages,table,consumed);
}
#else
void lofty_render(uint32_t *, const uint32_t *, unsigned, const uint32_t *);
void lofty_render_pages(uint32_t *, const uint32_t *, unsigned, const uint32_t *, uint32_t *);
#endif
void lofty_samples(uint32_t *, const uint32_t *, unsigned skip, size_t, const uint32_t *);
