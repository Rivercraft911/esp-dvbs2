// The transmitter's PIE kernels on the board, radio off, against portable C or independent
// oracles. Host builds compile no PIE assembly (no DVBS2_PIE, no __XTENSA__), so this is where
// the assembly the transmitter runs meets its references:
//   stages     every fast stage with PIE against the plain encoder (dvbs2.c): all normal and short
//              MODCODs, pilots on and off, each BCH, LDPC-pack and mapper variant, payloads and
//              phase and packed outputs at shifting alignments, guards around all owned memory
//   bb_inputs  BB with PIE at all 16 input offsets: GCS lengths around vector and word edges, TS
//              at every one of the 188 packet phases
//   xorshift   payload_xorshift8 (firmware/tx/main/payload_pie.S) against scalar xorshift32
//   fills      shaper fill kernels against shaper_fill_c, the QPSK quad shaper against
//              shaper_qpsk_c, shaper_copy_words against a word loop
//   ring       ring.c's unpack and quad-copy kernels against scalar models
//   ingress    the slot copy (ingress_pie.S) against memcpy, every source offset, reads watched
//   rx_ts      the receiver's TS deframer (dvbs2rx rx_ts.c), whose whole packets take the TS CRC-8
//              kernel: dvbs2_bb_ts frames back to their packets, frames at every start offset and
//              ending at a watched block boundary, one corrupted packet marked
//   qpsk_axes, shaper_axes, psk8_kernels: the host checks of those names, here on PIE code
// A region monitor of the debug assistant (soc/esp32s3/register/soc/assist_debug_reg.h) flags any
// core-0 read in the 256 bytes beyond either end of each BB input, rounded out to 16-byte blocks:
// a kernel may read the rest of a vector that holds input bytes, never a vector that holds none.
// With the input's end on a block boundary the check is exact.
//
//   (. ~/.espressif/tools/activate_idf_v6.1.sh && cd tests/tx_pie_bench && idf.py build flash -p PORT)
//   python3 host/board.py check --reset --timeout 600 --port PORT
//
// Any command line starts a run. Each part prints an @ record with its case count; a mismatch adds
// a record with "error" and its case. An assert in a reused host check aborts with its message.
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/usb_serial_jtag_vfs.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/assist_debug_reg.h"
#include "soc/system_reg.h"

#include "console.h"
#include "dvbs2.h"
#include "machine.h"
#include "oracles.h"
#include "ring.h"
#include "shaper.h"
#include "shaper_qpsk.h"
#include "shaper_taps.h"

#define INTERNAL (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define EXTERNAL (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

static uint32_t seed = 2026;
static uint32_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }

static void *take(size_t n, uint32_t caps)
{
    void *p = heap_caps_aligned_alloc(16, n, caps);
    if (!p) {
        printf("@{\"error\":\"allocation failed\",\"bytes\":%u}\n", (unsigned)n);
        abort();
    }
    return p;
}

/* ------------------------------------------------------------ reporting */

static const char *part;
static unsigned cases, errors, total_errors;
static int64_t started;
static const char *kinds[16]; // kinds of mismatch in this part, and how many of each
static unsigned seen[16];

static void begin(const char *name)
{
    part = name;
    cases = errors = 0;
    memset(kinds, 0, sizeof kinds);
    memset(seen, 0, sizeof seen);
    started = esp_timer_get_time();
}

static void end(void)
{
    printf("@{\"part\":\"%s\",\"cases\":%u,\"errors\":%u,\"ms\":%u,\"kinds\":{", part, cases, errors,
           (unsigned)((esp_timer_get_time() - started) / 1000));
    for (unsigned k = 0; k < 16 && kinds[k]; ++k) printf("%s\"%s\":%u", k ? "," : "", kinds[k], seen[k]);
    printf("}}\n");
    total_errors += errors;
}

// Count each kind of mismatch and print its first three cases.
static void miss(const char *what, unsigned a, unsigned b, unsigned c, unsigned d)
{
    unsigned k = 0;
    while (k < 16 && kinds[k] && strcmp(kinds[k], what)) ++k;
    if (k < 16 && !kinds[k]) kinds[k] = what;
    if (k < 16 && seen[k]++ < 3)
        printf("@{\"part\":\"%s\",\"error\":\"%s\",\"case\":[%u,%u,%u,%u]}\n", part, what, a, b, c, d);
    ++errors;
}

static bool same(const void *a, const void *b, size_t n) { return !memcmp(a, b, n); }

// Index of the first differing byte (n if none).
static unsigned first_diff(const uint8_t *a, const uint8_t *b, size_t n)
{
    size_t i = 0;
    while (i < n && a[i] == b[i]) ++i;
    return (unsigned)i;
}

static bool filled(const void *p, uint8_t v, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; ++i)
        if (b[i] != v) return false;
    return true;
}

/* ------------------------------------------------------------ read monitor */

// Core 0, area 0: the 256 bytes from `above`; area 1: the 256 bytes below `below`. Reads only;
// the raw status is polled and no interrupt is routed.
#define WATCH_READS (ASSIST_DEBUG_CORE_0_AREA_DRAM0_0_RD_ENA | ASSIST_DEBUG_CORE_0_AREA_DRAM0_1_RD_ENA)
#define WATCH_RAW (ASSIST_DEBUG_CORE_0_AREA_DRAM0_0_RD_RAW | ASSIST_DEBUG_CORE_0_AREA_DRAM0_1_RD_RAW)
static uint32_t flagged_pc;

static void watch(const void *below, const void *above)
{
    REG_WRITE(ASSIST_DEBUG_CORE_0_INTERRUPT_ENA_REG, 0);
    REG_WRITE(ASSIST_DEBUG_CORE_0_AREA_DRAM0_0_MIN_REG, (uintptr_t)above);
    REG_WRITE(ASSIST_DEBUG_CORE_0_AREA_DRAM0_0_MAX_REG, (uintptr_t)above + 255);
    REG_WRITE(ASSIST_DEBUG_CORE_0_AREA_DRAM0_1_MIN_REG, (uintptr_t)below - 256);
    REG_WRITE(ASSIST_DEBUG_CORE_0_AREA_DRAM0_1_MAX_REG, (uintptr_t)below - 1);
    REG_WRITE(ASSIST_DEBUG_CORE_0_INTERRUPT_CLR_REG, WATCH_RAW);
    REG_WRITE(ASSIST_DEBUG_CORE_0_INTERRUPT_CLR_REG, 0);
    REG_WRITE(ASSIST_DEBUG_CORE_0_INTERRUPT_ENA_REG, WATCH_READS);
}

// The raw bits since watch(): area 0 (above) in bit 0, area 1 (below) in bit 2.
static uint32_t watched(void)
{
    const uint32_t raw = REG_READ(ASSIST_DEBUG_CORE_0_INTERRUPT_RAW_REG) & WATCH_RAW;
    REG_WRITE(ASSIST_DEBUG_CORE_0_INTERRUPT_ENA_REG, 0);
    if (raw) flagged_pc = REG_READ(ASSIST_DEBUG_CORE_0_AREA_PC_REG);
    return raw;
}

#define MARGIN 256
static uint8_t *inbox; // MARGIN, 16-byte offset, the largest input, 16 bytes, MARGIN

// Copy n input bytes to `at` (0..15) past a 16-byte boundary; watch the blocks beyond both ends.
static const uint8_t *place(const uint8_t *src, size_t n, unsigned at)
{
    uint8_t *p = inbox + MARGIN + at;
    memcpy(p, src, n);
    watch(inbox + MARGIN, (const void *)(((uintptr_t)p + n + 15) & ~(uintptr_t)15));
    return p;
}

uint32_t dvbs2_pie_crc8_packet(const uint8_t *p, const uint8_t *cols); // ts_pie.S

// The monitor sees scalar and vector reads on both sides and stays quiet for reads inside.
static void monitor(void)
{
    begin("monitor");
    static uint8_t cols[12 * 8 * 16 + 16] __attribute__((aligned(16)));
    volatile uint8_t *b = inbox + MARGIN;
    uint32_t sink = 0;
    watch((const void *)b, (const void *)(b + 224));
    for (unsigned i = 0; i < 224; ++i) sink += b[i];
    sink += dvbs2_pie_crc8_packet((const uint8_t *)b, cols); // reads exactly b[0..224)
    const uint32_t inside = watched();
    watch((const void *)b, (const void *)(b + 224));
    sink += b[224];
    const uint32_t scalar_above = watched();
    watch((const void *)b, (const void *)(b + 224));
    sink += b[-1];
    const uint32_t scalar_below = watched();
    watch((const void *)b, (const void *)(b + 208));
    sink += dvbs2_pie_crc8_packet((const uint8_t *)b, cols); // its last block is b[208..224)
    const uint32_t vector_above = watched();
    watch((const void *)(b + 16), (const void *)(b + 256));
    sink += dvbs2_pie_crc8_packet((const uint8_t *)b, cols); // its first block is b[0..16)
    const uint32_t vector_below = watched();
    cases = 5;
    if (inside) miss("read inside flagged", inside, 0, 0, 0);
    if (scalar_above != ASSIST_DEBUG_CORE_0_AREA_DRAM0_0_RD_RAW) miss("scalar above", scalar_above, 0, 0, 0);
    if (scalar_below != ASSIST_DEBUG_CORE_0_AREA_DRAM0_1_RD_RAW) miss("scalar below", scalar_below, 0, 0, 0);
    if (vector_above != ASSIST_DEBUG_CORE_0_AREA_DRAM0_0_RD_RAW) miss("vector above", vector_above, 0, 0, 0);
    if (vector_below != ASSIST_DEBUG_CORE_0_AREA_DRAM0_1_RD_RAW) miss("vector below", vector_below, 0, 0, 0);
    (void)sink;
    end();
}

/* ------------------------------------------------------------ dvbs2 stages */

enum { V_C, V_FLASH, V_NIBBLE, V_RESIDENT, V_SPLIT, V_BCH, V_PACK, V_MAP, VARIANTS };
static const char *const variant_name[VARIANTS] = {
    "c", "flash", "nibble", "resident", "split", "bch", "pack", "map"};
static const char *const bch_names[] = {"pair", "split-pair", "byte", "nibble"};
#define GUARD 16

static struct {
    dvbs2_fast *f;
    dvbs2_workspace *ws, *plain;
    uint8_t *table, *bch, *pair, *payload, *out, *packed;
    uint8_t *ref_bb, *ref_fec, *ref_ph, *ref_packed, *ref_quads;
    size_t fast_bytes, table_bytes;
    unsigned ran[VARIANTS], bch_paths[4];
} s;

static size_t fec_bytes(const dvbs2_mode *m) { return m->short_frame ? DVBS2_SHORT_FEC_BYTES : DVBS2_FEC_BYTES; }

// Prepare variant v: 0 ready, 1 not offered for this mode, -1 refused.
static int setup(const dvbs2_mode *m, int pilots, unsigned v)
{
    unsigned flags = v == V_C ? 0 : 15;
    if (v == V_NIBBLE) flags |= DVBS2_BCH_NIBBLE;
    if (v == V_MAP) {
        if (m->bits_per_symbol == 2 && m->short_frame) return 1; // quads serve normal frames only
        flags |= m->bits_per_symbol == 3 ? DVBS2_MAP_SPARSE : DVBS2_MAP_QUADS;
    }
    if (dvbs2_fast_prepare(s.f, m, pilots, flags)) return -1;
    if (v == V_RESIDENT || v == V_SPLIT) {
        const size_t size = dvbs2_fast_cache_size(s.f);
        if (!size) return 1;
        if (size > s.table_bytes || dvbs2_fast_cache_tables(s.f, s.table)) return -1;
        if (v == V_SPLIT) {
            if (m->t == 8 || m->kbch % 16) return 1;
            if (dvbs2_fast_cache_bch_pair(s.f, s.pair)) return -1;
        }
    }
    if (v == V_BCH) { // byte table for t = 10/12, pair tables for t = 8 (8PSK at 4 sps)
        if (dvbs2_fast_cache_bch(s.f, s.bch)) return -1;
    }
    if (v == V_PACK && dvbs2_fast_cache_pack(s.f)) return 1;
    return 0;
}

// The plain encoder's BB (scrambled), FEC and, per pilot setting, phases and both packings.
static void reference(const dvbs2_mode *m, const uint8_t *src, size_t bytes, size_t dfl,
                      dvbs2_ts_state *ts)
{
    if (ts) dvbs2_bb_ts(s.plain, m, ts, src, bytes);
    else dvbs2_bb_gcs(s.plain, m, src, dfl);
    dvbs2_bb_scramble(s.plain, m);
    memcpy(s.ref_bb, s.plain->bits, m->kbch / 8);
    dvbs2_bch(s.plain, m);
    dvbs2_ldpc(s.plain, m);
    memcpy(s.ref_fec, s.plain->bits, fec_bytes(m));
}

static void reference_phases(const dvbs2_mode *m, int pilots)
{
    const size_t symbols = dvbs2_frame_symbols(m, pilots);
    dvbs2_map(s.plain, m, s.ref_ph + 90);
    dvbs2_plframe(m, pilots, s.ref_ph);
    dvbs2_pack(s.ref_ph, s.ref_packed, symbols);
    if (m->bits_per_symbol == 2) dvbs2_pack_quads(s.ref_ph, s.ref_quads, symbols);
}

// One frame through prepared variant v, every stage against the reference. BB input at `at`,
// phase and packed outputs at `o`; ts: the state before (advanced here) and after.
static void frame(const dvbs2_mode *m, int pilots, unsigned v, unsigned id, size_t bytes,
                  size_t dfl, dvbs2_ts_state *ts, const dvbs2_ts_state *ts_after, unsigned at,
                  unsigned o)
{
    const size_t used = m->short_frame ? DVBS2_SHORT_FAST_WORKSPACE_USED : DVBS2_FAST_WORKSPACE_USED;
    memset(s.ws, 0x5a, sizeof *s.ws);
    const uint8_t *in = place(s.payload, bytes, at);
    const int r = ts ? dvbs2_fast_bb_ts(s.f, s.ws, ts, in, bytes) : dvbs2_fast_bb_gcs(s.f, s.ws, in, dfl);
    if (watched()) miss("bb read outside input", id, v, at, flagged_pc);
    if (r) miss("bb refused", id, v, pilots, 0);
    if (ts && (ts->packet_pos != ts_after->packet_pos || ts->crc != ts_after->crc ||
               ts->previous_crc != ts_after->previous_crc))
        miss("ts state", id, v, pilots, 0);
    if (!same(s.ws->bits, s.ref_bb, m->kbch / 8)) miss("bb", id, v, pilots, at);
    dvbs2_fast_bch(s.f, s.ws);
    if (!same(s.ws->bits, s.ref_fec, m->nbch / 8)) miss("bch", id, v, pilots, 0);
    dvbs2_fast_ldpc(s.f, s.ws);
    if (!same(s.ws->bits, s.ref_fec, fec_bytes(m))) miss("ldpc", id, v, pilots, 0);
    if (!filled((uint8_t *)s.ws + used, 0x5a, sizeof *s.ws - used)) miss("workspace guard", id, v, pilots, 0);

    const size_t symbols = dvbs2_frame_symbols(m, pilots);
    memset(s.out, 0xa5, symbols + 2 * GUARD);
    dvbs2_fast_map(s.f, s.ws, s.out + GUARD + o);
    dvbs2_fast_pl(s.f, s.out + GUARD + o);
    if (!same(s.out + GUARD + o, s.ref_ph, symbols))
        miss("phases", id, v, pilots, first_diff(s.out + GUARD + o, s.ref_ph, symbols));
    if (!filled(s.out, 0xa5, GUARD + o) || !filled(s.out + GUARD + o + symbols, 0xa5, GUARD - o))
        miss("phase guard", id, v, pilots, o);

    const bool quads = v == V_MAP && m->bits_per_symbol == 2;
    const size_t packed = quads ? (symbols + 3) / 4 : symbols / 2;
    memset(s.packed, 0xa5, packed + 2 * GUARD);
    if (dvbs2_fast_map_packed(s.f, s.ws, s.packed + GUARD + o)) miss("packed refused", id, v, pilots, 0);
    dvbs2_fast_pl_packed(s.f, s.packed + GUARD + o);
    if (!same(s.packed + GUARD + o, quads ? s.ref_quads : s.ref_packed, packed))
        miss(quads ? "quads" : "packed", id, v, pilots,
             first_diff(s.packed + GUARD + o, quads ? s.ref_quads : s.ref_packed, packed));
    if (!filled(s.packed, 0xa5, GUARD + o) || !filled(s.packed + GUARD + o + packed, 0xa5, GUARD - o))
        miss("packed guard", id, v, pilots, o);

    if (!filled((uint8_t *)s.f + s.fast_bytes, 0xa5, GUARD) ||
        !filled(s.table + s.table_bytes, 0xa5, GUARD) || !filled(s.bch + DVBS2_BCH_CACHE_BYTES, 0xa5, GUARD) ||
        !filled(s.pair + DVBS2_BCH_PAIR_BYTES, 0xa5, GUARD))
        miss("context or table guard", id, v, pilots, 0);
    const void *table, *pair;
    const char *path = dvbs2_fast_bch_variant(s.f, &table, &pair);
    for (unsigned k = 0; k < 4; ++k) s.bch_paths[k] += v != V_C && !strcmp(path, bch_names[k]);
    ++s.ran[v];
    ++cases;
}

// Payload bytes; TS keeps a sync byte wherever the state puts a packet start.
static void payload(size_t bytes, const dvbs2_ts_state *ts)
{
    for (size_t i = 0; i < bytes; ++i) s.payload[i] = (uint8_t)rnd();
    if (ts)
        for (size_t k = ts->packet_pos ? 188u - ts->packet_pos : 0; k < bytes; k += 188) s.payload[k] = 0x47;
}

static void stages(void)
{
    begin("stages");
    // Mismatch cases start with 1000 * short + 10 * MODCOD + payload case.
    for (unsigned family = 0; family < 2; ++family)
        for (unsigned mc = 4; mc <= 17; ++mc) {
            const dvbs2_mode *m = family ? dvbs2_find_mode_short(mc) : dvbs2_find_mode(mc);
            if (!m) continue;
            const size_t cap = (m->kbch - 80) / 8;
            const unsigned errors_before = errors;
            dvbs2_ts_state ts = {0};
            // GCS: full, empty, 13 bits, three bits short of full; then three consecutive TS frames.
            for (unsigned c = 0; c < 7; ++c) {
                const unsigned id = 1000 * family + 10 * mc + c;
                const bool is_ts = c >= 4;
                const size_t dfl = is_ts ? 0 : (size_t[]){8 * cap, 0, 13, 8 * cap - 3}[c];
                const size_t bytes = is_ts ? cap : (dfl + 7) / 8;
                const dvbs2_ts_state before = ts;
                payload(bytes, is_ts ? &ts : NULL);
                reference(m, s.payload, bytes, dfl, is_ts ? &ts : NULL);
                for (int pilots = 0; pilots < 2; ++pilots) {
                    reference_phases(m, pilots);
                    for (unsigned v = 0; v < VARIANTS; ++v) {
                        const int r = setup(m, pilots, v);
                        if (r < 0) miss("prepare or cache refused", id, v, pilots, 0);
                        if (r) continue;
                        dvbs2_ts_state state = before;
                        frame(m, pilots, v, id, bytes, dfl, is_ts ? &state : NULL, &ts,
                              (c * 5 + v * 3 + pilots * 7 + mc) % 16,
                              (c * 3 + v * 5 + pilots * 11 + mc + family) % 16);
                    }
                }
            }
            if (errors > errors_before)
                printf("@{\"part\":\"stages\",\"modcod\":%u,\"short\":%s,\"mismatches\":%u}\n", mc,
                       family ? "true" : "false", errors - errors_before);
        }
    printf("@{\"part\":\"stages\",\"frames\":{");
    for (unsigned v = 0; v < VARIANTS; ++v) printf("%s\"%s\":%u", v ? "," : "", variant_name[v], s.ran[v]);
    printf("},\"bch_paths\":{");
    for (unsigned k = 0; k < 4; ++k) printf("%s\"%s\":%u", k ? "," : "", bch_names[k], s.bch_paths[k]);
    printf("}}\n");
    end();
}

// BB alone, at every input offset: GCS lengths around 16-byte, word and byte edges, and TS from
// every packet phase with arbitrary running CRCs. Every input is watched.
static void bb_inputs(void)
{
    begin("bb_inputs");
    static const unsigned picks[][2] = {{0, 4}, {0, 11}, {0, 13}, {0, 17}, {1, 4}, {1, 10}};
    for (unsigned k = 0; k < sizeof picks / sizeof *picks; ++k) {
        const dvbs2_mode *m = picks[k][0] ? dvbs2_find_mode_short(picks[k][1]) : dvbs2_find_mode(picks[k][1]);
        const size_t cap = (m->kbch - 80) / 8;
        if (dvbs2_fast_prepare(s.f, m, 1, 15)) { miss("prepare", k, 0, 0, 0); continue; }
        const size_t lengths[] = {1, 7, 8, 9, 15, 16, 17, 21, 22, 23, 38, 39, 127, 128, 129, 177, 8 * cap - 9,
                                  8 * cap - 1, 8 * cap};
        for (unsigned l = 0; l < sizeof lengths / sizeof *lengths; ++l)
            for (unsigned at = 0; at < 16; ++at) {
                const size_t dfl = lengths[l], bytes = (dfl + 7) / 8;
                payload(bytes, NULL);
                dvbs2_bb_gcs(s.plain, m, s.payload, dfl);
                dvbs2_bb_scramble(s.plain, m);
                const uint8_t *in = place(s.payload, bytes, at);
                const int r = dvbs2_fast_bb_gcs(s.f, s.ws, in, dfl);
                if (watched()) miss("gcs read outside input", k, l, at, flagged_pc);
                if (r || !same(s.ws->bits, s.plain->bits, m->kbch / 8)) miss("gcs", k, l, at, 0);
                ++cases;
            }
        for (unsigned phase = 0; phase < 188; ++phase)
            for (unsigned at = 0; at < 16; ++at) {
                // Any running CRC is reachable after the first byte; one byte past a sync it is 0.
                dvbs2_ts_state plain = {phase, (uint8_t)rnd(), phase == 1 ? 0 : (uint8_t)rnd()};
                dvbs2_ts_state fast = plain;
                payload(cap, &plain);
                dvbs2_bb_ts(s.plain, m, &plain, s.payload, cap);
                dvbs2_bb_scramble(s.plain, m);
                const uint8_t *in = place(s.payload, cap, at);
                const int r = dvbs2_fast_bb_ts(s.f, s.ws, &fast, in, cap);
                if (watched()) miss("ts read outside input", k, phase, at, flagged_pc);
                if (r || !same(s.ws->bits, s.plain->bits, m->kbch / 8) || fast.packet_pos != plain.packet_pos ||
                    fast.crc != plain.crc || fast.previous_crc != plain.previous_crc)
                    miss("ts", k, phase, at, 0);
                ++cases;
            }
    }
    end();
}

static void dvbs2_parts(void)
{
    size_t biggest = 0;
    s.fast_bytes = dvbs2_fast_size();
    s.f = take(s.fast_bytes + GUARD, INTERNAL);
    for (unsigned family = 0; family < 2; ++family)
        for (unsigned mc = 4; mc <= 17; ++mc) {
            const dvbs2_mode *m = family ? dvbs2_find_mode_short(mc) : dvbs2_find_mode(mc);
            if (m && !dvbs2_fast_prepare(s.f, m, 1, 15) && dvbs2_fast_cache_size(s.f) > biggest)
                biggest = dvbs2_fast_cache_size(s.f);
        }
    s.table_bytes = (biggest + 15) & ~(size_t)15;
    s.ws = take(sizeof *s.ws, INTERNAL);
    s.table = take(s.table_bytes + GUARD, INTERNAL);
    s.bch = take(DVBS2_BCH_CACHE_BYTES + GUARD, INTERNAL);
    s.pair = take(DVBS2_BCH_PAIR_BYTES + GUARD, INTERNAL);
    s.out = take(DVBS2_MAX_SYMBOLS + 2 * GUARD, INTERNAL);
    s.packed = take(DVBS2_MAX_SYMBOLS / 2 + 2 * GUARD, INTERNAL);
    inbox = take(2 * MARGIN + 16 + DVBS2_FEC_BYTES + 16, INTERNAL);
    s.plain = take(sizeof *s.plain, EXTERNAL);
    s.payload = take(DVBS2_FEC_BYTES, EXTERNAL);
    s.ref_bb = take(DVBS2_FEC_BYTES, EXTERNAL);
    s.ref_fec = take(DVBS2_FEC_BYTES, EXTERNAL);
    s.ref_ph = take(DVBS2_MAX_SYMBOLS, EXTERNAL);
    s.ref_packed = take(DVBS2_MAX_SYMBOLS / 2, EXTERNAL);
    s.ref_quads = take(DVBS2_MAX_SYMBOLS / 4 + 1, EXTERNAL);
    memset((uint8_t *)s.f + s.fast_bytes, 0xa5, GUARD);
    memset(s.table + s.table_bytes, 0xa5, GUARD);
    memset(s.bch + DVBS2_BCH_CACHE_BYTES, 0xa5, GUARD);
    memset(s.pair + DVBS2_BCH_PAIR_BYTES, 0xa5, GUARD);
    memset(s.ran, 0, sizeof s.ran);
    memset(s.bch_paths, 0, sizeof s.bch_paths);
    monitor();
    stages();
    bb_inputs();
    void *all[] = {s.f, s.ws, s.table, s.bch, s.pair, s.out, s.packed, inbox, s.plain, s.payload,
                   s.ref_bb, s.ref_fec, s.ref_ph, s.ref_packed, s.ref_quads};
    for (unsigned i = 0; i < sizeof all / sizeof *all; ++i) heap_caps_free(all[i]);
}

/* ------------------------------------------------------------ synthetic payload */

void payload_xorshift8(uint8_t *dst, unsigned quads, uint32_t *lanes, unsigned stride);

static uint32_t xorshift_step(uint32_t x) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x; }

// Eight lanes, each a block of 4*quads bytes `stride` apart, from word-aligned destinations.
static void xorshift(void)
{
    begin("xorshift");
    static const unsigned quad_counts[] = {1, 2, 3, 4, 5, 8, 16, 61, 227, 454};
    const size_t room = 32 + 8 * (4 * 454 + 20);
    uint8_t *got = take(room, INTERNAL), *want = take(room, EXTERNAL);
    uint32_t lanes[8] __attribute__((aligned(16))), after[8];
    for (unsigned q = 0; q < sizeof quad_counts / sizeof *quad_counts; ++q)
        for (unsigned gap = 0; gap < 3; ++gap)
            for (unsigned at = 0; at < 16; at += 4) {
                const unsigned quads = quad_counts[q], stride = 4 * quads + (unsigned[]){0, 4, 20}[gap];
                for (unsigned j = 0; j < 8; ++j) lanes[j] = j == 0 ? 1 : j == 1 ? UINT32_MAX : rnd();
                memset(got, 0xa5, room);
                memset(want, 0xa5, room);
                for (unsigned j = 0; j < 8; ++j) {
                    uint32_t x = lanes[j];
                    for (unsigned i = 0; i < 4 * quads; ++i) want[16 + at + j * stride + i] = (uint8_t)(x = xorshift_step(x));
                    after[j] = x;
                }
                payload_xorshift8(got + 16 + at, quads, lanes, stride);
                if (!same(got, want, room)) miss("bytes or guards", quads, stride, at, 0);
                if (!same(lanes, after, sizeof after)) miss("lane states", quads, stride, at, 0);
                ++cases;
            }
    heap_caps_free(got);
    heap_caps_free(want);
    end();
}

/* ------------------------------------------------------------ shaper */

#define WORST_PEAK 718.4 // firmware/tx/main/dvbtx.c: the taps' conservative axis bound
#define SYMBOLS 256
enum { CODES, RAW, RADIX4 };

// Whole and partial symbols from shaper_samples(_codes)_profile against shaper_fill_c over the
// same phases, with history, at each input byte offset, skip and count.
static void fill_case(const char *name, const shaper_table_t *t, unsigned kind, uint32_t *got, uint32_t *want)
{
    static uint8_t ref[8 + SYMBOLS + 8], in[8 + SYMBOLS + 8];
    const unsigned n = t->n;
    const bool qpsk = t->bits == 2;
    for (unsigned trial = 0; trial < 4; ++trial) {
        for (unsigned i = 0; i < sizeof ref; ++i) {
            const unsigned k = qpsk ? 2 * (rnd() & 3) + 1 : rnd() & 7;
            ref[i] = 16 * k; // as shaper_fill_c reads phases
            in[i] = kind == RAW ? k : kind == RADIX4 ? 16 * (k >> 1) : 16 * k;
        }
        static const unsigned counts[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 13, 14, 37, 64, 97, 200};
        for (unsigned c = 0; c < sizeof counts / sizeof *counts; ++c)
            for (unsigned first = 0; first < 4; ++first) {
                const unsigned nsym = counts[c];
                shaper_fill_c(want, ref + 8 + first, (int)nsym, t);
                const unsigned skips[] = {0, 1, n / 2, n - 1};
                for (unsigned k = 0; k < 4; ++k)
                    for (unsigned tail = 0; tail < 2; ++tail) {
                        const unsigned skip = skips[k];
                        const size_t count = nsym * n - skip - (tail ? (n - 1) / 2 + 1 : 0);
                        if ((int)count <= 0 || count > nsym * n - skip) continue;
                        // Whole symbols start 16-byte aligned, after the partial first one.
                        uint32_t *dst = got + 64 - (skip ? n - skip : 0);
                        for (unsigned i = 0; i < 64 + SYMBOLS * 40 + 64; ++i) got[i] = 0xdeadbeef;
                        (kind == RAW ? shaper_samples_profile : shaper_samples_codes_profile)(
                            dst, in + 8 + first, skip, count, t, NULL, NULL);
                        if (!same(dst, want + skip, 4 * count)) miss(name, nsym, first, skip, (unsigned)count);
                        for (uint32_t *g = got; g < got + 64 + SYMBOLS * 40 + 64; ++g)
                            if ((g < dst || g >= dst + count) && *g != 0xdeadbeef) {
                                miss("fill guard", nsym, first, skip, (unsigned)count);
                                break;
                            }
                        ++cases;
                    }
            }
    }
}

// shaper_qpsk_samples (shaper_qpsk_pie.S bulk, C edges) against shaper_qpsk_c, including
// destinations off the vector alignment the bulk path needs.
static void quads_case(unsigned n, uint32_t *got, uint32_t *want)
{
    static uint8_t src[2 + 128 + 2];
    shaper_qpsk_build(shaper_store, n, 508);
    for (unsigned i = 0; i < sizeof src; ++i) src[i] = (uint8_t)rnd();
    const unsigned block = 4 * n;
    static const unsigned group_counts[] = {1, 2, 3, 4, 7, 8, 31, 128};
    for (unsigned g = 0; g < sizeof group_counts / sizeof *group_counts; ++g) {
        const unsigned groups = group_counts[g];
        shaper_qpsk_c(want, src + 2, groups, shaper_store, n);
        const unsigned skips[] = {0, 1, block / 2, block - 1};
        for (unsigned k = 0; k < 4; ++k)
            for (unsigned shift = 0; shift < 4; ++shift)
                for (unsigned tail = 0; tail < 2; ++tail) {
                    const unsigned skip = skips[k];
                    const size_t count = groups * block - skip - (tail ? block / 2 + 1 : 0);
                    if ((int)count <= 0 || count > groups * block - skip) continue;
                    uint32_t *dst = got + 64 - ((block - skip) & 3) + shift;
                    for (unsigned i = 0; i < 64 + 128 * 16 + 64; ++i) got[i] = 0xdeadbeef;
                    shaper_qpsk_samples(dst, src + 2, skip, count, shaper_store, n);
                    if (!same(dst, want + skip, 4 * count)) miss(n == 3 ? "qpsk3" : "qpsk4", groups, skip, shift, (unsigned)count);
                    for (uint32_t *p = got; p < got + 64 + 128 * 16 + 64; ++p)
                        if ((p < dst || p >= dst + count) && *p != 0xdeadbeef) {
                            miss("quads guard", groups, skip, shift, (unsigned)count);
                            break;
                        }
                    ++cases;
                }
    }
}

static void fills(void)
{
    begin("fills");
    uint32_t *got = take(4 * (64 + SYMBOLS * 40 + 64), INTERNAL);
    uint32_t *want = take(4 * SYMBOLS * 40, INTERNAL);
    static const struct { const char *name; unsigned n, bits, kind; } kinds[] = {
        {"fill4", 4, 3, CODES}, {"fill8", 8, 3, CODES}, {"fill40", 40, 2, CODES},
        {"fill40_raw", 40, 2, RAW}, {"fill4_qpsk", 4, 2, RADIX4}, {"fill8_qpsk", 8, 2, RADIX4},
        {"short4", 4, 3, CODES}};
    for (unsigned k = 0; k < sizeof kinds / sizeof *kinds; ++k) {
        shaper_table_t t;
        if (!strcmp(kinds[k].name, "short4")) {
            shaper_short_build(&t, shaper_store, 508);
        } else {
            const unsigned n = kinds[k].n;
            const int32_t *source = n == 40 ? shaper_taps_qpsk_n40_a35 : n == 8 ? shaper_taps_8psk_n8_a35
                                                                             : shaper_taps_8psk_n4_a35;
            static int32_t taps[360];
            for (unsigned i = 0; i < 9 * n; ++i) taps[i] = (int32_t)((double)source[i] * 508 / WORST_PEAK);
            shaper_build(&t, shaper_store, taps, (int)n, (int)kinds[k].bits);
        }
        fill_case(kinds[k].name, &t, kinds[k].kind, got, want);
    }
    quads_case(3, got, want);
    quads_case(4, got, want);
    // Word copies: every source and destination word offset in a vector, lengths through pairs.
    uint32_t *src = want;
    for (unsigned i = 0; i < 128; ++i) src[i] = rnd();
    for (unsigned d = 0; d < 4; ++d)
        for (unsigned so = 0; so < 4; ++so)
            for (unsigned words = 0; words <= 80; ++words) {
                for (unsigned i = 0; i < 128; ++i) got[i] = 0xdeadbeef;
                shaper_copy_words(got + 8 + d, src + so, words);
                bool ok = same(got + 8 + d, src + so, 4 * words);
                for (unsigned i = 0; i < 128; ++i)
                    if ((i < 8 + d || i >= 8 + d + words) && got[i] != 0xdeadbeef) ok = false;
                if (!ok) miss("copy_words", d, so, words, 0);
                ++cases;
            }
    heap_caps_free(got);
    heap_caps_free(want);
    end();
}

/* ------------------------------------------------------------ ring */

static unsigned nibble(const uint8_t *p, size_t j) { return (p[j / 2] >> (4 * (j & 1))) & 15; }
static unsigned quad(const uint8_t *p, size_t j) { return (p[j / 4] >> (6 - 2 * (j & 3))) & 3; }

static void ring(void)
{
    begin("ring");
    static uint8_t packed[600 + 16] __attribute__((aligned(16)));
    static uint8_t got[1200 + 64] __attribute__((aligned(16))), want[sizeof got];
    static const unsigned lengths[] = {0, 1, 2, 3, 31, 32, 33, 47, 48, 49, 63, 64, 65, 66, 100, 257, 1000};
    // Packed phases 0..7, two per byte: raw, codes 16*k, and QPSK (odd k) codes 16*(k>>1).
    for (unsigned form = 0; form < 3; ++form) {
        for (unsigned i = 0; i < sizeof packed; ++i)
            packed[i] = (uint8_t)(form == 2 ? (rnd() & 0x66) | 0x11 : rnd() & 0x77);
        for (unsigned at = 0; at < 34; ++at)
            for (unsigned d = 0; d < 16; ++d)
                for (unsigned l = 0; l < sizeof lengths / sizeof *lengths; ++l) {
                    const size_t n = lengths[l];
                    memset(got, 0xa5, sizeof got);
                    memset(want, 0xa5, sizeof want);
                    for (size_t i = 0; i < n; ++i) {
                        const unsigned v = nibble(packed, at + i);
                        want[16 + d + i] = form == 0 ? v : form == 1 ? v << 4 : (v >> 1) << 4;
                    }
                    (form == 0 ? ring_unpack : form == 1 ? ring_unpack_codes : ring_unpack_qpsk)(
                        got + 16 + d, packed, at, n);
                    if (!same(got, want, sizeof got)) miss(form == 0 ? "unpack" : form == 1 ? "unpack_codes" : "unpack_qpsk", at, d, (unsigned)n, 0);
                    ++cases;
                }
    }
    for (unsigned i = 0; i < sizeof packed; ++i) packed[i] = (uint8_t)rnd();
    // Two-bit codes from any source position to any destination position.
    for (unsigned at = 0; at < 36; ++at)
        for (unsigned to = 0; to < 4 * 20; to += 3)
            for (unsigned l = 0; l < sizeof lengths / sizeof *lengths; ++l) {
                const size_t n = lengths[l];
                for (unsigned i = 0; i < sizeof got; ++i) got[i] = want[i] = (uint8_t)(i * 37);
                for (size_t i = 0; i < n; ++i) {
                    const size_t j = to + i;
                    const unsigned shift = 6 - 2 * (j & 3);
                    want[j / 4] = (uint8_t)((want[j / 4] & ~(3u << shift)) | quad(packed, at + i) << shift);
                }
                ring_copy_quads(got, to, packed, at, n);
                if (!same(got, want, sizeof got)) miss("copy_quads", at, to, (unsigned)n, 0);
                ++cases;
            }
    end();
}

/* ------------------------------------------------------------ ingress */

void ingress_copy_pie(uint8_t *dst, const uint8_t *src, unsigned blocks);

// 16 * blocks bytes from every source offset to an aligned destination; the kernel may read only
// the 16-byte blocks that hold source bytes.
static void ingress(void)
{
    begin("ingress");
    const size_t room = 2 * MARGIN + 16 + 84 * 16 + 16;
    uint8_t *src = take(room, INTERNAL), *got = take(84 * 16 + 32, INTERNAL);
    for (size_t i = 0; i < room; ++i) src[i] = (uint8_t)rnd();
    static const unsigned block_counts[] = {1, 2, 3, 4, 7, 16, 82, 83};
    for (unsigned b = 0; b < sizeof block_counts / sizeof *block_counts; ++b)
        for (unsigned at = 0; at < 16; ++at) {
            const unsigned blocks = block_counts[b];
            const uint8_t *from = src + MARGIN + at;
            memset(got, 0xa5, 84 * 16 + 32);
            watch(src + MARGIN, (const void *)(((uintptr_t)from + 16 * blocks + 15) & ~(uintptr_t)15));
            ingress_copy_pie(got + 16, from, blocks);
            if (watched()) miss("read outside source", blocks, at, flagged_pc, 0);
            if (!same(got + 16, from, 16 * blocks) || !filled(got, 0xa5, 16) ||
                !filled(got + 16 + 16 * blocks, 0xa5, 16))
                miss("copy or guards", blocks, at, 0, 0);
            ++cases;
        }
    heap_caps_free(src);
    heap_caps_free(got);
    end();
}

/* ------------------------------------------------------------ rx_ts */

#include "dvbs2rx.h"

static const uint8_t *rx_stream;   /* the packets the frames carry, syncs as sent */
static unsigned rx_emitted, rx_wrong;

static void rx_emit(void *ctx, const uint8_t *p)
{
    (void)ctx;
    rx_wrong += !same(p, rx_stream + 188 * rx_emitted, 188);
    ++rx_emitted;
}

// The transmitter's TS framing (dvbs2_bb_ts, BBHEADER and data field) into the receiver's deframer:
// the packets come back exact and in order. Even frames start at every offset; odd ones end their
// data field on a 16-byte boundary, and any read past the field or before the BBHEADER's block
// is flagged.
static void rx_ts(void)
{
    begin("rx_ts");
    static const unsigned picks[][2] = {{1, 16}, {1, 4}, {0, 11}};
    const unsigned frames = 24, packets = 24 * 7264 / 188 + 2;
    uint8_t *stream = take(188 * packets, EXTERNAL);
    uint8_t *box = take(2 * MARGIN + 32 + 10 + DVBS2_FEC_BYTES, INTERNAL);
    dvbs2_workspace *plain = take(sizeof *plain, EXTERNAL);
    rx_ts_t *t = take(rx_ts_size(), INTERNAL);
    for (unsigned i = 0; i < 188 * packets; ++i) stream[i] = i % 188 ? (uint8_t)rnd() : 0x47;
    rx_stream = stream;
    for (unsigned k = 0; k < sizeof picks / sizeof *picks; ++k)
        for (unsigned corrupt = 0; corrupt < 2; ++corrupt) {
            const dvbs2_mode *m = picks[k][0] ? dvbs2_find_mode_short(picks[k][1]) : dvbs2_find_mode(picks[k][1]);
            const size_t cap = (m->kbch - 80) / 8;
            dvbs2_ts_state ts = {0};
            rx_ts_init(t, 0, rx_emit, NULL);
            rx_emitted = rx_wrong = 0;
            for (unsigned f = 0; f < frames; ++f) {
                dvbs2_bb_ts(plain, m, &ts, stream + f * cap, cap);
                if (corrupt && f == 3) plain->bits[10 + 400] ^= 4;
                const unsigned at = f % 2 ? (unsigned)(16 - (10 + cap) % 16) % 16 : (f * 7) % 16;
                uint8_t *frame = box + MARGIN + 16 + at;
                memcpy(frame, plain->bits, 10 + cap);
                watch((const void *)((uintptr_t)frame & ~(uintptr_t)15),
                      (const void *)(((uintptr_t)frame + 10 + cap + 15) & ~(uintptr_t)15));
                rx_ts_frame(t, frame, (unsigned)cap, f);
                if (watched()) miss("read outside frame", picks[k][1], f, at, flagged_pc);
                memset(frame, 0xa5, 10 + cap);  /* the receiver reuses the slot */
                ++cases;
            }
            rx_ts_stats_t st;
            rx_ts_stats(t, &st);
            const unsigned want = (unsigned)((frames * cap - 1) / 188);
            if (rx_emitted != want || st.crc_errors != corrupt || rx_wrong != corrupt)
                miss("packets or crc", picks[k][1], rx_emitted, st.crc_errors, rx_wrong);
        }
    heap_caps_free(stream);
    heap_caps_free(box);
    heap_caps_free(plain);
    heap_caps_free(t);
    end();
}

/* ------------------------------------------------------------ reused host checks */

// machine.c's host oracles (machine.h), for the reused checks.
void machine_render(uint32_t *dst, const uint32_t *signs, unsigned blocks, const uint32_t *table)
{
    const unsigned shift[] = {3, 2, 0};
    while (blocks--) {
        const uint32_t w = *signs++;
        for (unsigned v = 0; v < 3; ++v) {
            const uint32_t *i = table + (v * 256 + ((w >> (shift[v] + 4)) & 255)) * 4;
            const uint32_t *q = table + ((v + 3) * 256 + ((w >> (shift[v] + 20)) & 255)) * 4;
            for (unsigned p = 0; p < 4; ++p) *dst++ = i[p] | q[p];
        }
    }
}

uint32_t machine_expand(uint32_t *dst, const uint32_t *axes, uint32_t history)
{
    unsigned pending = 0;
    for (unsigned k = 0; k < MACHINE_PAGE_AXIS_WORDS; ++k)
        for (int shift = 15; shift >= 0; --shift) {
            const uint32_t signs = axes[k] >> shift;
            history = ((history << 1) & 0x7ff07ff0) | (signs & 1) << 4 | (signs & 0x10000) << 4;
            if (++pending == 5) {
                *dst++ = history;
                pending = 0;
            }
        }
    return history;
}

void qpsk_axes_check(void);
void machine_expand_check(void);
void machine_axis_check(void);
void shaper_axes_main(void);
int psk8_kernels_main(void);

static void reused(void)
{
    begin("qpsk_axes");
    qpsk_axes_check();
    machine_axis_check();
    end();
    begin("shaper_axes");
    shaper_axes_main();
    end();
    begin("psk8_kernels");
    psk8_kernels_main();
    end();
}

void app_main(void)
{
    usb_serial_jtag_driver_config_t usb = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb.rx_buffer_size = usb.tx_buffer_size = 4096;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
    usb_serial_jtag_vfs_use_driver();
    static FILE output = FDEV_SETUP_STREAM(console_put, NULL, NULL, _FDEV_SETUP_WRITE);
    stdout = &output;
    setvbuf(stdin, NULL, _IONBF, 0);
    REG_SET_BIT(SYSTEM_CPU_PERI_CLK_EN_REG, SYSTEM_CLK_EN_ASSIST_DEBUG);
    REG_CLR_BIT(SYSTEM_CPU_PERI_RST_EN_REG, SYSTEM_RST_EN_ASSIST_DEBUG);
    char line[64];
    for (;;) {
        if (!fgets(line, sizeof line, stdin)) {
            vTaskDelay(1);
            continue;
        }
        total_errors = 0;
        seed = 2026;
        dvbs2_parts();
        xorshift();
        fills();
        ring();
        ingress();
        rx_ts();
        reused();
        printf("@{\"part\":\"all\",\"errors\":%u,\"firmware\":\"%s\"}\n@{\"done\":true}\n", total_errors,
               esp_app_get_description()->version);
    }
}
