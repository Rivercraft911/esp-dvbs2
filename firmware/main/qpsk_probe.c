// Radio-off probe of the four-symbol kernel and whole-bank overhead. No dvbtx.c wiring.
#include "qpsk_probe.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/usb_serial_jtag.h"
#include "dvbs2.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "radio_s3.h"
#include "shaper.h"
#include "shaper_qpsk.h"
#include "station_id.h"
#include "station_store.h"

#define CACHE 768u  // 640 normal overlap + first bank's extra 128-word lead
#define WINDOW (2 + (RADIO_S3_BANK_WORDS + 22) / 12)
#define RATE RADIO_S3_DAC_RATE_80M

typedef struct {
    const uint8_t *source;
    size_t source_bytes;
    uint64_t end, shaped, reused;
    unsigned n;
    uint8_t win[WINDOW];
    uint32_t cache[CACHE] __attribute__((aligned(16)));
} probe_t;

static IRAM_ATTR esp_err_t fill(uint32_t *dst, uint64_t first, size_t count, void *ctx)
{
    probe_t *p = ctx;
    if (first < p->end) {
        if (p->end-first > CACHE) return ESP_FAIL;
        size_t take = p->end-first < count ? p->end-first : count;
        const size_t at = first % CACHE, head = take < CACHE-at ? take : CACHE-at;
        shaper_copy_words(dst,p->cache+at,head);
        shaper_copy_words(dst+head,p->cache,take-head);
        dst += take, first += take, count -= take;
        p->reused += take;
    }
    if (!count) return ESP_OK;
    const unsigned row = 4*p->n;
    const uint64_t block = first / row;
    const unsigned skip = first % row;
    const size_t need = 2 + (skip+count+row-1)/row;
    if (need > sizeof p->win) return ESP_FAIL;
    // Byte copies include packed history and at most one partial four-symbol block.
    // Preserve global block phase when a QPSK frame ends halfway through a byte.
    size_t done = 0;
    if (block < 2) {
        done = 2-block;
        memset(p->win,0,done);  // phase 1 before the first symbol
    }
    size_t at = (block+done-2) % p->source_bytes;
    while (done < need) {
        const size_t take = need-done < p->source_bytes-at ? need-done : p->source_bytes-at;
        memcpy(p->win+done,p->source+at,take);
        done += take, at = 0;
    }
    shaper_qpsk_samples(dst,p->win+2,skip,count,shaper_store,p->n);
    const size_t save = count < CACHE ? count : CACHE;
    at = (first+count-save) % CACHE;
    const size_t head = save < CACHE-at ? save : CACHE-at;
    shaper_copy_words(p->cache+at,dst+count-save,head);
    shaper_copy_words(p->cache,dst+count-save+head,save-head);
    p->end = first+count;
    p->shaped += count;
    return ESP_OK;
}

// Staging only. A streaming encoder is deliberately outside this probe.
static uint8_t *prepare(unsigned modcod, unsigned frames, bool internal, size_t *length)
{
    const dvbs2_mode *mode = dvbs2_find_mode(modcod);
    const size_t symbols = dvbs2_frame_symbols(mode,1), bytes = (mode->kbch-80)/8;
    *length = symbols*frames/4;
    uint8_t *packed = heap_caps_malloc(*length, (internal ? MALLOC_CAP_INTERNAL : MALLOC_CAP_SPIRAM)
                                                 | MALLOC_CAP_8BIT);
    dvbs2_workspace *ws = heap_caps_malloc(sizeof *ws,MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    dvbs2_fast *fast = heap_caps_aligned_alloc(16,dvbs2_fast_mode_size(mode),
                                              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t *payload = heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *raw = heap_caps_malloc(symbols,MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool ok = packed && ws && fast && payload && raw;
    if (ok) ok = !dvbs2_fast_prepare(fast,mode,1,15 | DVBS2_BCH_NIBBLE);
    uint32_t x = 2026;
    size_t at = 0;
    uint8_t w = 0;
    for (unsigned f = 0; ok && f < frames; ++f) {
        for (size_t i = 0; i < bytes; ++i) {
            x ^= x << 13, x ^= x >> 17, x ^= x << 5;
            payload[i] = x;
        }
        station_payload_header(payload,bytes,station_store_get());
        ok = !dvbs2_fast_bb_gcs(fast,ws,payload,8*bytes);
        if (!ok) break;
        dvbs2_fast_bch(fast,ws);
        dvbs2_fast_ldpc(fast,ws);
        dvbs2_fast_map(fast,ws,raw);
        dvbs2_fast_pl(fast,raw);
        for (size_t i = 0; i < symbols; ++i,++at) {
            w = (w << 2) | (raw[i] >> 1);
            if ((at & 3) == 3) packed[at/4] = w;
        }
    }
    heap_caps_free(raw); heap_caps_free(payload); heap_caps_free(fast); heap_caps_free(ws);
    if (!ok) { heap_caps_free(packed); packed = NULL; }
    return packed;
}

typedef struct { uint8_t buf[4096]; size_t used; uint32_t crc; } dump_t;
static void flush(dump_t *d)
{
    d->crc = esp_rom_crc32_le(d->crc,d->buf,d->used);
    usb_serial_jtag_write_bytes(d->buf,d->used,portMAX_DELAY);
    d->used = 0;
}
static esp_err_t sink(const uint32_t *bank, uint64_t first, size_t count, void *ctx)
{
    dump_t *d = ctx;
    for (size_t i = 0; i < count; ++i) {
        const uint32_t w = bank[(first+i) % RADIO_S3_BANK_WORDS];
        const int16_t iq[] = {(int32_t)(w << 22) >> 22,(int32_t)(w << 12) >> 22};
        memcpy(d->buf+d->used,iq,4);
        d->used += 4;
        if (d->used == sizeof d->buf) flush(d);
    }
    return ESP_OK;
}

void qpsk_probe_command(const char *cmd, char *args)
{
    char *save, *name = strtok_r(args," \t",&save), *duration = strtok_r(NULL," \t",&save);
    char *code = strtok_r(NULL," \t",&save), *extra = strtok_r(NULL," \t",&save), *end;
    const bool dump = !strcmp(cmd,"dvbdump");
    const unsigned n = name && !strcmp(name,"qpsk3") ? 3 : name && !strcmp(name,"qpsk4") ? 4 : 0;
    unsigned long count = duration ? strtoul(duration,&end,10) : 0;
    bool valid = n && duration && *duration && !*end && count >= 1 && count <= (dump ? 40 : 60);
    unsigned long modcod = code ? strtoul(code,&end,10) : 11;
    valid &= !code || (*code && !*end);
    if (!valid || extra || modcod < 4 || modcod > 11) {
        printf("@{\"cmd\":\"%s\",\"error\":\"usage: %s qpsk3|qpsk4 <1..%u> [4..11 modcod]\"}\n",
               cmd,cmd,dump ? 40 : 60);
        return;
    }
    probe_t *p = heap_caps_aligned_alloc(16,sizeof *p,MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    dump_t *d = dump ? heap_caps_malloc(sizeof *d,MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    size_t bytes;
    // Two complete frames align the repeated period to a four-symbol block. A dump stages
    // an even number of frames and emits only the requested contiguous prefix.
    uint8_t *source = prepare(modcod,dump ? (count+1)&~1u : 2,!dump,&bytes);
    if (!p || (dump && !d) || !source || shaper_qpsk_build(shaper_store,n,128)) {
        printf("@{\"cmd\":\"%s\",\"error\":\"qpsk probe allocation/build failed\"}\n",cmd);
        goto done;
    }
    memset(p,0,sizeof *p);
    p->n = n, p->source = source, p->source_bytes = bytes;
    const uint64_t samples = count * (uint64_t)dvbs2_frame_symbols(dvbs2_find_mode(modcod),1) * n;
    esp_err_t err;
    if (dump) {
        memset(d,0,sizeof *d);
        printf("@{\"cmd\":\"dvbdump\",\"binary_bytes\":%" PRIu64
               ",\"format\":\"int16 I/Q LE\",\"sample_rate\":%u}\n",samples*4,RATE);
        fflush(stdout);
        radio_s3_dry_stats_t stats;
        err = radio_s3_dry_run_rate(fill,p,samples,RATE,sink,d,&stats);
        if (d->used) flush(d);
        printf("@{\"cmd\":\"dvbdump\",\"frames\":%lu,\"crc32\":%" PRIu32
               ",\"overlap_mismatches\":%" PRIu32 ",\"fills\":%" PRIu32,count,d->crc,
               stats.overlap_mismatches,stats.fills);
    } else {
        radio_s3_stats_t stats;
        err = radio_s3_paced_run_rate(count,RATE,fill,p,&stats);
        printf("@{\"cmd\":\"dvbpace\",\"seconds\":%lu,\"fills\":%" PRIu32
               ",\"max_fill_cycles\":%" PRIu32 ",\"min_fill_slack_cycles\":%" PRIu32
               ",\"late_fills\":%" PRIu32 ",\"fatal_late\":%" PRIu32,
               count,stats.fills,stats.max_fill_cycles,stats.min_fill_slack_cycles,
               stats.late_fills,stats.fatal_late);
    }
    printf(",\"experiment\":\"qpsk-fill\",\"encoder_timed\":false,\"sps\":%u,\"dac_rate\":%u,"
           "\"modcod\":%lu,\"callsign\":\"%s\",\"source_memory\":\"%s\",\"source_bytes\":%u,"
           "\"state_bytes\":%u,\"state_heap_bytes\":%u,\"source_heap_bytes\":%u,"
           "\"table_bytes\":%u,\"free_internal\":%u,\"largest_internal\":%u,"
           "\"shaped_words\":%" PRIu64 ",\"reused_words\":%" PRIu64 ",\"status\":\"%s\"}\n",
           n,RATE,modcod,station_store_get(),dump ? "psram" : "internal",(unsigned)bytes,
           (unsigned)sizeof *p,(unsigned)heap_caps_get_allocated_size(p),
           (unsigned)heap_caps_get_allocated_size(source),(unsigned)(QPSK_TABLE_WORDS(n)*4),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           p->shaped,p->reused,
           esp_err_to_name(err));
done:
    heap_caps_free(source); heap_caps_free(d); heap_caps_free(p);
}
