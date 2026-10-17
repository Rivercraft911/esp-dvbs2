#pragma once
#include <stddef.h>
#include <stdint.h>

// Five-symbol .35 pulse, three samples/symbol, four symbols/block.
#define LOFTY_TABLE_WORDS 9216
#define LOFTY_BLOCKS 128
#define LOFTY_STORED 38
#define LOFTY_PAGES 49
#define LOFTY_WAKE 2
typedef struct {
    uint32_t words[LOFTY_BLOCKS+4]; // owned trailing prefetch guard
} __attribute__((aligned(16))) lofty_page;
// Four phases -> three scaled offsets. Previous is the preceding four phases.
// Source has 16 readable guard bytes. Arbitrary destination word alignment.
uint32_t lofty_prepare(uint32_t *, const uint16_t *, unsigned, uint32_t previous);
uint32_t lofty_prepare_pie(uint32_t *, const uint16_t *, unsigned vectors, uint32_t);
int lofty_build(uint32_t *, unsigned peak);
void lofty_render(uint32_t *, const uint32_t *, unsigned, const uint32_t *);
void lofty_render_pie(uint32_t *, const uint32_t *, unsigned, const uint32_t *);
void lofty_samples(uint32_t *, const uint32_t *, unsigned skip, size_t, const uint32_t *);
