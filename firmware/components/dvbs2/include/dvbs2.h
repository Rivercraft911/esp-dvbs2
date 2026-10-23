#ifndef DVBS2_H
#define DVBS2_H

#include <stddef.h>
#include <stdint.h>

/* Normal (64,800 bit) and short (16,200 bit) FECFRAMEs; short is QPSK MODCOD 4..10 and 8PSK MODCOD 12..16.
 * Bits are packed MSB first; phase k means exp(j*k*pi/4).
 * SIS/CCM, roll-off 0.35, no ISSY/NPD, PL scrambling code 0.
 * Use mode pointers from dvbs2_find_mode (normal), dvbs2_find_mode_short or the
 * tables; the frame size travels with the mode (short_frame). Payload/input and
 * phase output buffers must not overlap the workspace or TS state. */
#define DVBS2_FEC_BYTES 8100
#define DVBS2_MAX_SYMBOLS 33282
#define DVBS2_DUMMY_SYMBOLS 3330
/* 5.5.1, 5.5.4: MODCOD 0, no pilots, 36 slots of pi/4 carrier, PL scrambled.
 * Output is phases 0..7, as for data frames. */
void dvbs2_dummy(uint8_t *output);

typedef struct {
    const char *rate;
    uint16_t kbch, nbch;
    uint8_t modcod, bits_per_symbol, t;
    /* q = 360-bit column height (Table 7), at most 90. short_frame = 1: 16,200-bit FECFRAME
     * (Table 5b), t = 12 with 168 parity bits. Both fit the byte that keeps the struct 12 bytes. */
    uint8_t q : 7, short_frame : 1;
} dvbs2_mode;

extern const dvbs2_mode dvbs2_modes[];
extern const size_t dvbs2_mode_count;
const dvbs2_mode *dvbs2_find_mode(unsigned modcod);
/* Short FECFRAME modes: MODCOD 4..10 (QPSK 1/2 .. 8/9) and 12..16 (8PSK 3/5 .. 8/9), else NULL. Short Kbch is 7,032
 * (1/2) .. 14,232 (8/9) bits; 3/4 carries (11,712 - 80) / 8 = 1,454 payload bytes.
 * A short QPSK PLFRAME has 8,370 symbols with pilots, 8,190 without; 8PSK 5,598 and 5,490. */
extern const dvbs2_mode dvbs2_modes_short[];
extern const size_t dvbs2_mode_short_count;
const dvbs2_mode *dvbs2_find_mode_short(unsigned modcod);
#define DVBS2_SHORT_FEC_BYTES 2025
#define DVBS2_SHORT_MAX_SYMBOLS 8370
size_t dvbs2_frame_symbols(const dvbs2_mode *mode, int pilots);

typedef struct {
    uint8_t bits[DVBS2_FEC_BYTES];
    uint32_t parity[90][12]; /* q rows, 360 useful LSB-first bits per row */
} dvbs2_workspace;

/* Fast stages use bits[] plus bounded vector lookahead, never reference parity[].
 * Bytes [8144, sizeof(dvbs2_workspace)) may hold unrelated data while ONLY fast
 * stages run. The plain LDPC encoder needs parity[] and cannot share this tail.
 * 8100 bytes round up to 8112, then 32 guard bytes; test_encoder_pie checks it.
 * Fast-only callers may allocate DVBS2_FAST_WORKSPACE_USED aligned bytes,
 * followed by any caller-owned window. Never pass that allocation to plain stages. */
#define DVBS2_FAST_WORKSPACE_USED 8144
/* Same rule for short frames: 2,025 bytes round up to 2,032, then 32 guard bytes. The first
 * bytes of any dvbs2_workspace serve a short mode; plain stages still need all of it. */
#define DVBS2_SHORT_FAST_WORKSPACE_USED 2064

typedef struct {
    uint16_t packet_pos; /* raw TS byte offset, 0..187 */
    uint8_t previous_crc, crc;
} dvbs2_ts_state;

/* Zero initialization starts a TS stream (the first replacement CRC is zero).
 * Each call consumes exactly (kbch-80)/8 original TS bytes. Packet sync 0x47 is
 * replaced by the previous packet CRC. State continues across BBFRAMEs.
 * BB functions return 0 on success, -1 for invalid input; on error they do not
 * modify the workspace or TS state. GCS accepts arbitrary dfl_bits, including 0. */
int dvbs2_bb_gcs(dvbs2_workspace *ws, const dvbs2_mode *mode,
                const uint8_t *payload, size_t dfl_bits);
/* RO is the two-bit MATYPE-1 code: 0=0.35, 1=0.25, 2=0.20 (5.1.6). */
int dvbs2_bb_gcs_ro(dvbs2_workspace *ws, const dvbs2_mode *mode,
                   const uint8_t *payload, size_t dfl_bits, unsigned ro);
int dvbs2_bb_ts(dvbs2_workspace *ws, const dvbs2_mode *mode,
               dvbs2_ts_state *state, const uint8_t *input, size_t input_bytes);

/* Public stages permit bit-for-bit checks and per-stage cycle measurements.
 * Call in this order after bb_gcs/bb_ts. bits[] contains BBFRAME, then scrambled
 * BBFRAME, BCH codeword, and LDPC codeword respectively. */
void dvbs2_bb_scramble(dvbs2_workspace *ws, const dvbs2_mode *mode);
void dvbs2_bch(dvbs2_workspace *ws, const dvbs2_mode *mode);
void dvbs2_ldpc(dvbs2_workspace *ws, const dvbs2_mode *mode);
/* Fuses the 8PSK column interleaver with mapping; QPSK has no interleaver. */
void dvbs2_map(const dvbs2_workspace *ws, const dvbs2_mode *mode,
               uint8_t *data_phases);
/* Map to output+90 first. Expands in place, adds header and pilots, scrambles
 * the body. Allocate dvbs2_frame_symbols(mode,pilots) bytes. */
void dvbs2_plframe(const dvbs2_mode *mode, int pilots, uint8_t *output);

/* Fast encoder (fast.c): shared immutable tables in flash, mutable scratch in
 * internal RAM. Allocate dvbs2_fast_size() bytes, 16-byte aligned (about 17 KB),
 * and prepare for one mode and pilot setting. Workspace must also be 16-byte
 * aligned. The 4-sps stream uses an internal workspace and resident BCH table
 * to avoid flash/PSRAM cache conflicts; QPSK n8 does too. Legacy n40 retains PSRAM.
 * Use internal payload and packed output buffers for the cycle target. Both CPU
 * cores share the data cache, so external-memory timing needs a board check.
 * The pie mask selects ESP32-S3 vector kernels (pie.S) per stage; without PIE
 * support the mask is ignored and portable C runs. dvbs2_fast_pie() tells
 * which kernels are in use; dvbs2_fast_ldxq_bias() reports the measured
 * EE.LDXQ.32 address offset (0 or 4 bytes, -1 if unused). Stages keep
 * ws->bits in the canonical format: bb_* writes the scrambled BBFRAME, then
 * bch and ldpc as above. map writes the mapped and scrambled data symbols to
 * their PLFRAME positions and may scribble on the others; pl then writes the
 * header and pilots. output: dvbs2_frame_symbols(mode, pilots) bytes, any
 * alignment. */
typedef struct dvbs2_fast dvbs2_fast;
/* NIBBLE uses register-based BCH rows for PSRAM workspaces; default t=10/12
 * byte rows are cached internally, while t=8 uses paired byte rows in flash.
 * MAP_SPARSE keeps the
 * 8PSK mapper's hot cache footprint at 512 B beside a PSRAM workspace.
 * MAP_QUADS selects the QPSK block shaper's four-symbol bytes.
 * RO_20 selects GCS BBHEADER roll-off 0.20; the default is 0.35. */
/* VCM clears CCM in GCS MATYPE-1. TS VCM requires Annex D adaptation, not implemented. */
enum { DVBS2_BCH_NIBBLE = 16, DVBS2_MAP_SPARSE = 256, DVBS2_MAP_QUADS = 512,
       DVBS2_RO_20 = 1024, DVBS2_VCM = 2048 };
/* QPSK only: MAP_QUADS makes the packed APIs emit four phase>>1 codes per byte,
 * oldest in bits 7:6. The last byte's unused low bits are zero. */
void dvbs2_pack_quads(const uint8_t *phases, uint8_t *packed, size_t symbols);
/* Borrow (kbch-80)/8 bytes of encoder scratch for GCS payload generation. BB
 * consumes it before BCH/LDPC overwrite it. Never use concurrently with a stage. */
uint8_t *dvbs2_fast_payload(dvbs2_fast *f);
enum { DVBS2_PIE_BB = 1, DVBS2_PIE_BCH = 2, DVBS2_PIE_LDPC = 4, DVBS2_PIE_MAP = 8 };
size_t dvbs2_fast_size(void);
/* Smaller allocation valid only for this mode.
 * Allocate internal output first, then context, then payload to
 * preserve a large free block in the transmitter build. Reprepare with the same mode
 * and either pilot setting; allocate fast_size() to switch freely. */
size_t dvbs2_fast_mode_size(const dvbs2_mode *mode);
int dvbs2_fast_prepare(dvbs2_fast *f, const dvbs2_mode *mode, int pilots, unsigned pie);
/* Optional resident byte-BCH table for t = 10/12. After prepare, copy once to
 * an owned 16-byte-aligned buffer of this size, outside context/workspace/output.
 * It stays valid until the next prepare or cache_bch call. NULL restores flash.
 * Returns -1 for invalid context/alignment or t = 8. No allocation is hidden. */
#define DVBS2_BCH_CACHE_BYTES 8192
int dvbs2_fast_cache_bch(dvbs2_fast *f, uint8_t *table);
/* Optional t=10/12 pairs, even byte counts only. Call after cache_tables,
 * with a separate aligned low table. Uses the resident table's high padding;
 * both buffers live until prepare/cache_bch. No internal allocation. */
#define DVBS2_BCH_PAIR_BYTES 4096
int dvbs2_fast_cache_bch_pair(dvbs2_fast *, uint8_t *low);
/* Optional complete hot-data cache (PRBS, Gold, BCH, LDPC
 * program/columns and GCS header CRC). After prepare, allocate cache_size()
 * aligned internal bytes, then cache_tables(). Buffer remains owned by caller
 * until the context is released or prepared again. Size 0 means unsupported. */
size_t dvbs2_fast_cache_size(const dvbs2_fast *f);
int dvbs2_fast_cache_tables(dvbs2_fast *f, uint8_t *table);
/* Optional disjoint tail for LDPC program, columns and header CRC. Allocate
 * cache_size()-cache_tail_size() bytes at table and cache_tail_size() at tail.
 * Both buffers have the same alignment/lifetime as cache_tables. table NULL caches
 * the tail alone (a receiver's LDPC check needs only that and the BCH table). */
size_t dvbs2_fast_cache_tail_size(const dvbs2_fast *f);
int dvbs2_fast_cache_split(dvbs2_fast *f, uint8_t *table, uint8_t *tail);
unsigned dvbs2_fast_pie(const dvbs2_fast *f);
int dvbs2_fast_ldxq_bias(const dvbs2_fast *f);
int dvbs2_fast_bb_gcs(const dvbs2_fast *f, dvbs2_workspace *ws,
                      const uint8_t *payload, size_t dfl_bits);
int dvbs2_fast_bb_ts(const dvbs2_fast *f, dvbs2_workspace *ws, dvbs2_ts_state *state,
                     const uint8_t *input, size_t input_bytes);
void dvbs2_fast_bch(dvbs2_fast *f, dvbs2_workspace *ws);
void dvbs2_fast_ldpc(dvbs2_fast *f, dvbs2_workspace *ws);
void dvbs2_fast_map(dvbs2_fast *f, const dvbs2_workspace *ws, uint8_t *output);
void dvbs2_fast_pl(const dvbs2_fast *f, uint8_t *output);

/* Default complete-frame path. Prepare f once with the desired PIE mask (15
 * for all kernels, 0 for portable C). No allocation or preparation per frame.
 * GCS: ts == NULL, input_bits is DFL (including partial bytes). TS: ts != NULL,
 * input_bits must equal kbch-80; consumes input_bits/8 bytes and advances ts.
 * Output has dvbs2_frame_symbols(mode, pilots) bytes, phases 0..7. Same buffer
 * and non-overlap rules as the stages. Invalid input leaves ws, ts and output
 * unchanged. A context and workspace belong to one encoder at a time. */
int dvbs2_encode_frame(dvbs2_fast *f, dvbs2_workspace *ws, dvbs2_ts_state *ts,
                       const uint8_t *payload, size_t input_bits, uint8_t *output);

/* QPSK and 8PSK frames packed two symbols per byte, for rings in internal RAM: byte i =
 * phase[2i] | phase[2i + 1] << 4, dvbs2_frame_symbols(mode, pilots) / 2 bytes
 * (every frame length is even). map_packed and pl_packed write the same symbols
 * as map and pl without a full-frame phase buffer; they use the context's
 * scratch and any output alignment. PIE maps directly to packed bytes unless
 * MAP_SPARSE is selected for 8PSK.
 * QPSK always uses its direct packed mapper. The fixed header and pilots are
 * packed once by prepare. dvbs2_pack packs an even count of phases 0..7,
 * for example a dummy PLFRAME. */
int dvbs2_fast_map_packed(dvbs2_fast *f, const dvbs2_workspace *ws, uint8_t *packed);
void dvbs2_fast_pl_packed(const dvbs2_fast *f, uint8_t *packed);
int dvbs2_encode_frame_packed(dvbs2_fast *f, dvbs2_workspace *ws, dvbs2_ts_state *ts,
                              const uint8_t *payload, size_t input_bits, uint8_t *packed);
void dvbs2_pack(const uint8_t *phases, uint8_t *packed, size_t symbols);

/* Streaming mapper state, kept across frames, initially zero. at reaches frame_symbols
 * at EOF; a partial group survives EOF. */
typedef struct {
    uint32_t history;
    unsigned pending, at;
} dvbs2_sign_state;
/* QPSK FEC bytes to chronological quad bytes in the context scratch. */
int dvbs2_fast_sign_start(dvbs2_fast *f, const dvbs2_workspace *ws, dvbs2_sign_state *s);
/* QPSK axis queue: requires MAP_QUADS. Each output word holds sixteen I-negative bits in 15:0
 * and Q in 31:16, oldest first, including header and pilots. pending counts symbols modulo 16. */
int dvbs2_fast_axis_start(dvbs2_fast *, const dvbs2_workspace *, dvbs2_sign_state *);
unsigned dvbs2_fast_axis_read(const dvbs2_fast *, dvbs2_sign_state *, uint32_t *, unsigned words);

// 8PSK: four chronological phases per 12-bit word. Partial groups survive EOF.
// table is 1024 caller-owned internal bytes; prepare first, then cache once.
#define DVBS2_LOFTY_CACHE_BYTES 1024
int dvbs2_fast_lofty_cache(dvbs2_fast *, uint32_t *table);
// ws->bits must remain unchanged through the last read: mapping is incremental.
int dvbs2_fast_lofty_start(dvbs2_fast *, const dvbs2_workspace *, dvbs2_sign_state *);
// Borrow up to *blocks framed words, with 16 readable guard bytes; advance at.
// Consume the view before the next read, encoder stage or prepare.
const uint16_t *dvbs2_fast_lofty_read(dvbs2_fast *, dvbs2_sign_state *, unsigned *blocks);

#endif
