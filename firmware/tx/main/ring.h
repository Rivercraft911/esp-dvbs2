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
#define RING_CHOICES 8      // > ceil((16380*5/12 + 12)/3330) + 2, even with only dummies
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
    uint8_t packed;               // 1: unpack raw phases, 2: codes 16*k, 3: QPSK 16*(k>>1), 4: four codes/byte
} ring_t;

void ring_init(ring_t *r, uint8_t *slots, uint32_t nslots, const uint8_t *filler,
               uint32_t frame_symbols);
// Set up to RING_MAX_SLOTS pointers before either core starts. Each owns frame_symbols bytes.
void ring_init_slots(ring_t *r, uint8_t *const *slots, uint32_t nslots,
                     const uint8_t *filler, uint32_t frame_symbols);
// Set before the first read. Defaults to frame_symbols.
void ring_filler_length(ring_t *r, uint32_t symbols);
// Retain history + ceil(maximum bank overlap / sps) bytes in internal storage.
// This lets slots be freed as soon as every later read of them is covered by cache.
// Include the first bank's extra lead in maximum overlap. Set before the first fill.
void ring_cache(ring_t *r, uint8_t *storage, size_t symbols);
// Slots and filler hold frames packed by dvbs2_pack (two phases per byte, low nibble first);
// reads unpack them. Each slot and the filler need 16 readable bytes after their last byte, and
// reads are fastest when dst + symbol index is even (see ring_window). Set before the first fill.
void ring_packed(ring_t *r);
// Same packed source, but ring_window emits the shaper's legacy codes 16*k.
void ring_packed_codes(ring_t *r);
// Symbols [at, at + n) of a packed frame, one per byte. Vector loop when dst + at is even.
void ring_unpack(uint8_t *dst, const uint8_t *packed, size_t at, size_t n);
void ring_unpack_codes(uint8_t *dst, const uint8_t *packed, size_t at, size_t n);

// Encoder: where frame e goes, or NULL while a later read may still need that slot.
uint8_t *ring_slot(ring_t *r, uint32_t e);
// Encoder: frame e is complete. Frames are published in order.
void ring_publish(ring_t *r, uint32_t e);
// VCM lengths live in a caller-owned RING_MAX_SLOTS array, initially zero.
// Pass the same array to ring_window_vcm. Lengths are released with their frames.
void ring_publish_frame(ring_t *r, uint32_t e, uint32_t symbols, uint32_t *lengths);

// Fill: the symbols a shaper needs for samples [first, first + count) at sps samples per symbol,
// starting `history` symbols before the first one, into dst (room bytes). Symbols before 0 read
// as 0. With a packed ring, pass dst with the parity of first / sps - history (an even history:
// of first / sps) so every symbol lands at an address of its own parity. Returns the number
// of symbols after the history, or -1 if they do not fit or the read reaches a released
// position (a bug: stop the run). Then releases what no later fill can need:
// the fills follow the bank schedule (radio_s3.h), so no later fill starts before the start of
// the bank being filled, and that is at least first + count - bank_words.
int ring_window(ring_t *r, uint8_t *dst, size_t room, uint64_t first, size_t count,
                unsigned sps, unsigned history, unsigned bank_words);

// Variable-length frames. Use ring_window for fixed-length frames.
int ring_window_vcm(ring_t *, uint8_t *, size_t, uint64_t, size_t, unsigned, unsigned, unsigned, const uint32_t *);

// Compact two-bit QPSK codes for the 4/8-sps shaper, with the same source padding requirements.
void ring_packed_qpsk(ring_t *r);
void ring_unpack_qpsk(uint8_t *dst, const uint8_t *packed, size_t at, size_t n);

// Four QPSK codes/byte (phase>>1, high pair first), including frame-boundary
// nibble shifts. Set r->packed=4 before reads. ring_window uses byte room, rounds
// its first symbol down to a four-symbol boundary and retains 12..15 history
// symbols in four cache bytes. cache_symbols > 16 retains 24..27 symbols
// for the fractional block shaper, requiring seven cache bytes. Source buffers need sixteen readable guard bytes.
void ring_copy_quads(uint8_t *dst, size_t to, const uint8_t *src, size_t at, size_t n);
