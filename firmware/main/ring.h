#pragma once

// Frame ring between the encoder on one core and the bank fill on the other.
//
// The encoder finishes PL frames in order and writes frame e into slot e % nslots. The fill plays
// them in variable-length positions: a data frame has frame_symbols symbols; a dummy has
// filler_symbols symbols. Each retained position records its absolute start and end. The
// first time the fill reads position p it takes the next finished frame, or the filler frame if
// the encoder is late (an underrun), and keeps that choice, so the bank overlap re-reads the same
// symbols. Frames are never skipped or reordered: an underrun only inserts the filler, and the
// payload stays one contiguous stream.
//
// Only the encoder writes `published` and only the fill writes `freed`. Each reads the other's
// counter with acquire ordering and writes its own with release ordering (memw on the Xtensa
// cores; the S3's data cache is shared by both cores, TRM v1.8 section 4.3.3.2). The fill side
// never blocks and never calls the operating system.

#include <stddef.h>
#include <stdint.h>

#define RING_FILLER UINT32_MAX
#define RING_CHOICES 8      // > ceil((16380/4 + 8)/3330) + 2, even with only dummies
#define RING_MAX_SLOTS 8

typedef struct {
    uint8_t *slots[RING_MAX_SLOTS]; // separate allocations are allowed
    const uint8_t *filler;        // played when the encoder is late
    uint32_t frame_symbols;
    uint32_t filler_symbols;
    uint32_t nslots;
    uint32_t published;           // encoder: frames finished
    uint32_t freed;               // fill: frames no later read needs
    // Fill side only.
    uint32_t next_position;       // positions below this have a choice
    uint32_t next_frame;          // the next encoder frame to play
    uint32_t release_position;    // positions below this are released
    uint32_t choice[RING_CHOICES];
    uint64_t start[RING_CHOICES], end[RING_CHOICES];
    uint64_t chosen_end;          // first symbol not assigned to a position
    uint32_t underruns;           // positions that got the filler
    int64_t min_lead;             // fewest symbols ready beyond a read, over all reads
    uint8_t *cache;               // optional copied overlap, owned by fill side
    size_t cache_symbols;
    int64_t cache_first, cache_end;
} ring_t;

void ring_init(ring_t *r, uint8_t *slots, uint32_t nslots, const uint8_t *filler,
               uint32_t frame_symbols);
// Set up to RING_MAX_SLOTS pointers before either core starts. Each owns frame_symbols bytes.
void ring_init_slots(ring_t *r, uint8_t *const *slots, uint32_t nslots,
                     const uint8_t *filler, uint32_t frame_symbols);
// Set before the first read; defaults to frame_symbols for legacy callers.
void ring_filler_length(ring_t *r, uint32_t symbols);
// Retain history + ceil(maximum bank overlap / sps) bytes in internal storage.
// This lets slots be freed as soon as every later read of them is covered by cache.
// Include the first bank's extra lead in maximum overlap. Set before the first fill.
void ring_cache(ring_t *r, uint8_t *storage, size_t symbols);

// Encoder: where frame e goes, or NULL while a later read may still need that slot.
uint8_t *ring_slot(ring_t *r, uint32_t e);
// Encoder: frame e is complete. Frames are published in order.
void ring_publish(ring_t *r, uint32_t e);

// Fill: the symbols a shaper needs for samples [first, first + count) at sps samples per symbol,
// starting `history` symbols before the first one, into dst (room bytes). Symbols before 0 read
// as 0. Returns the number of symbols after the history, or -1 if they do not fit or the read
// reaches a released position (a bug: stop the run). Then releases what no later fill can need:
// the fills follow the bank schedule (radio_s3.h), so no later fill starts before the start of
// the bank being filled, and that is at least first + count - bank_words.
int ring_window(ring_t *r, uint8_t *dst, size_t room, uint64_t first, size_t count,
                unsigned sps, unsigned history, unsigned bank_words);
