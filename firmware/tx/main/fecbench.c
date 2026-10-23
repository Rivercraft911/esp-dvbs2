// fecbench: time the fast BCH and LDPC stages on one frame in internal RAM, the way a receiver
// re-encodes hard decisions to check them. Input is the xorshift32 stream (seed 2026) as
// Kbch/8 bytes of scrambled BBFRAME; the result CRC lets the host compare with host/dvbs2.py.
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "run_memory.h"
#include "esp_rom_crc.h"
#include "dvbs2.h"

#define RUNS 100
#define GUARD 64

typedef struct { uint32_t first, min, max; uint64_t sum; } stat_t;

static void add(stat_t *s, unsigned run, uint32_t cycles)
{
    if (!run) { s->first = cycles; s->min = UINT32_MAX; return; }
    s->sum += cycles;
    if (cycles < s->min) s->min = cycles;
    if (cycles > s->max) s->max = cycles;
}

static void show(const char *name, const stat_t *s)
{
    printf("\"%s\":{\"first\":%" PRIu32 ",\"min\":%" PRIu32 ",\"mean\":%" PRIu32 ",\"max\":%" PRIu32 "}",
           name, s->first, s->min, (uint32_t)(s->sum / (RUNS - 1)), s->max);
}

void fecbench_command(unsigned modcod, bool short_frame)
{
    const dvbs2_mode *mode = short_frame ? dvbs2_find_mode_short(modcod) : dvbs2_find_mode(modcod);
    if (!mode) {
        printf("@{\"cmd\":\"fecbench\",\"error\":\"usage: fecbench <modcod 4..17> [--short (4..10, 12..16)]\"}\n");
        return;
    }
    const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t used = short_frame ? DVBS2_SHORT_FAST_WORKSPACE_USED : DVBS2_FAST_WORKSPACE_USED;
    const size_t fec = (short_frame ? DVBS2_SHORT_FEC_BYTES : DVBS2_FEC_BYTES);
    run_reset();
    dvbs2_fast *f = run_take(dvbs2_fast_mode_size(mode), 16, internal);
    uint8_t *raw = run_take(used + GUARD, 16, internal);
    uint8_t *table = run_take(DVBS2_BCH_CACHE_BYTES, 16, internal);
    if (!f || !raw || !table) {
        printf("@{\"cmd\":\"fecbench\",\"error\":\"allocation failed\"}\n");
        return;
    }
    dvbs2_workspace *ws = (dvbs2_workspace *)raw;
    printf("@{\"cmd\":\"fecbench\",\"modcod\":%u,\"short\":%s,\"kbch\":%u,\"nbch\":%u,\"context_bytes\":%u,"
           "\"workspace_used\":%u,\"runs\":%u,\"variants\":[", modcod, short_frame ? "true" : "false",
           mode->kbch, mode->nbch, (unsigned)dvbs2_fast_mode_size(mode), (unsigned)used, RUNS);
    for (unsigned variant = 0; variant < 2; ++variant) {
        if (dvbs2_fast_prepare(f, mode, 1, 15) || (variant && dvbs2_fast_cache_bch(f, table))) {
            printf("%s{\"error\":\"prepare failed\"}", variant ? "," : "");
            continue;
        }
        stat_t bch = {0}, ldpc = {0};
        uint32_t crc = 0;
        for (unsigned run = 0; run < RUNS; ++run) {
            memset(raw, 0xa5, used + GUARD);
            uint32_t x = 2026;
            for (unsigned i = 0; i < mode->kbch / 8; ++i) {
                x ^= x << 13; x ^= x >> 17; x ^= x << 5;
                ws->bits[i] = (uint8_t)x;
            }
            const uint32_t t0 = esp_cpu_get_cycle_count();
            dvbs2_fast_bch(f, ws);
            const uint32_t t1 = esp_cpu_get_cycle_count();
            dvbs2_fast_ldpc(f, ws);
            const uint32_t t2 = esp_cpu_get_cycle_count();
            add(&bch, run, t1 - t0);
            add(&ldpc, run, t2 - t1);
            if (!run) crc = esp_rom_crc32_le(0, ws->bits, fec);
        }
        bool guard = true;
        for (unsigned i = used; i < used + GUARD; ++i) guard &= raw[i] == 0xa5;
        printf("%s{\"bch_table\":\"%s\",\"crc32\":%" PRIu32 ",\"guard_intact\":%s,", variant ? "," : "",
               variant ? "resident" : "flash", crc, guard ? "true" : "false");
        show("bch", &bch); printf(","); show("ldpc", &ldpc); printf("}");
    }
    printf("]}\n");
}
