// Normal-frame encoder timing. The source and the phase-byte CRC are reproducible on the host.
// Only encoding is timed; source generation, CRC, printing and the between-frame yield are not.
// Interrupts stay enabled, so raw totals include interrupt service during a frame. A pinned task
// keeps every CCOUNT subtraction on the same CPU. This command never starts the radio.
//
// Implementations, chosen per run so one image gives the A/B comparison:
//   plain  the reference stages in dvbs2.c
//   c      fast.c in portable C (bb_scramble is part of bb, so it reads 0)
//   pie    fast.c with the ESP32-S3 vector kernels from pie.S. Before the timed frames of each
//          mode, a pie_check record compares every PIE stage with the C stage on one frame.
//   legacy also selects the original compact mapper. nibble/sram select the BCH controls
//   documented in dvbs2.h; all
//   report impl:"pie", bch_variant:<name>. Copy time is inside BCH timing.
// Preparing the fast tables for each mode happens before the warmup frame and is not timed.

#include "fecbench.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dvbs2.h"
#include "esp_app_desc.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define FRAMES 5
#define PILOTS 1
#define STAGES 6

static uint32_t source_byte(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x & 0xff;
}

static void fill_payload(uint8_t *payload, size_t bytes, int ts, size_t offset, uint32_t *state)
{
    for (size_t i = 0; i < bytes; ++i) {
        payload[i] = source_byte(state);
        if (ts && (offset + i) % 188 == 0) {
            payload[i] = 0x47;
        }
    }
}

// IEEE CRC32, the same byte convention as Python zlib.crc32(output_phase_bytes).
static uint32_t phase_crc32(const uint8_t *bytes, size_t count)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < count; ++i) {
        crc ^= bytes[i];
        for (unsigned b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static int encode_fast(dvbs2_fast *f, dvbs2_workspace *ws, const dvbs2_mode *mode,
                       dvbs2_ts_state *ts, const uint8_t *payload, uint8_t *output,
                       uint32_t cycles[STAGES], uint32_t *total)
{
    const size_t payload_bytes = (mode->kbch - 80) / 8;
    const uint32_t start = esp_cpu_get_cycle_count();
    uint32_t before = esp_cpu_get_cycle_count();
    const int rc = ts ? dvbs2_fast_bb_ts(f, ws, ts, payload, payload_bytes)
                      : dvbs2_fast_bb_gcs(f, ws, payload, payload_bytes * 8);
    cycles[0] = esp_cpu_get_cycle_count() - before;
    cycles[1] = 0;
    if (rc != 0) {
        return rc;
    }
    before = esp_cpu_get_cycle_count();
    dvbs2_fast_bch(f, ws);
    cycles[2] = esp_cpu_get_cycle_count() - before;
    before = esp_cpu_get_cycle_count();
    dvbs2_fast_ldpc(f, ws);
    cycles[3] = esp_cpu_get_cycle_count() - before;
    before = esp_cpu_get_cycle_count();
    dvbs2_fast_map(f, ws, output);
    cycles[4] = esp_cpu_get_cycle_count() - before;
    before = esp_cpu_get_cycle_count();
    dvbs2_fast_pl(f, output);
    cycles[5] = esp_cpu_get_cycle_count() - before;
    *total = esp_cpu_get_cycle_count() - start;
    return 0;
}

static int encode(dvbs2_fast *f, dvbs2_workspace *ws, const dvbs2_mode *mode,
                  dvbs2_ts_state *ts, const uint8_t *payload, uint8_t *output,
                  uint32_t cycles[STAGES], uint32_t *total)
{
    if (f) {
        return encode_fast(f, ws, mode, ts, payload, output, cycles, total);
    }
    const size_t payload_bytes = (mode->kbch - 80) / 8;
    const uint32_t start = esp_cpu_get_cycle_count();
    uint32_t before = esp_cpu_get_cycle_count();
    const int rc = ts ? dvbs2_bb_ts(ws, mode, ts, payload, payload_bytes)
                      : dvbs2_bb_gcs(ws, mode, payload, payload_bytes * 8);
    cycles[0] = esp_cpu_get_cycle_count() - before;
    if (rc != 0) {
        return rc;
    }
    before = esp_cpu_get_cycle_count();
    dvbs2_bb_scramble(ws, mode);
    cycles[1] = esp_cpu_get_cycle_count() - before;
    before = esp_cpu_get_cycle_count();
    dvbs2_bch(ws, mode);
    cycles[2] = esp_cpu_get_cycle_count() - before;
    before = esp_cpu_get_cycle_count();
    dvbs2_ldpc(ws, mode);
    cycles[3] = esp_cpu_get_cycle_count() - before;
    before = esp_cpu_get_cycle_count();
    dvbs2_map(ws, mode, output + 90);
    cycles[4] = esp_cpu_get_cycle_count() - before;
    before = esp_cpu_get_cycle_count();
    dvbs2_plframe(mode, PILOTS, output);
    cycles[5] = esp_cpu_get_cycle_count() - before;
    *total = esp_cpu_get_cycle_count() - start;
    return 0;
}

static const char *const impl_names[] = {"plain", "c", "pie", "legacy", "nibble", "sram"};
static unsigned impl;

// Before timing a pie run: feed one GCS frame through each PIE stage and the portable C stage
// from the same input. Returns a mask of stages that differ: 1 bb, 2 bch, 4 ldpc, 8 map + pl.
static unsigned check_pie(dvbs2_fast *f, const dvbs2_mode *mode, unsigned pie,
                          dvbs2_workspace *c, dvbs2_workspace *v, uint8_t *oc, uint8_t *ov,
                          const uint8_t *payload)
{
    const size_t bits = (mode->kbch - 80) / 8 * 8, symbols = dvbs2_frame_symbols(mode, PILOTS);
    const size_t length[3] = {mode->kbch / 8, mode->nbch / 8, DVBS2_FEC_BYTES};
    unsigned bad = 0;
    for (unsigned stage = 0; stage < 4; ++stage) {
        for (unsigned use = 0; use < 2; ++use) {
            dvbs2_workspace *ws = use ? v : c;
            if (dvbs2_fast_prepare(f, mode, PILOTS, use ? pie : 0) != 0) {
                return 15;
            }
            if (stage == 0) {
                dvbs2_fast_bb_gcs(f, ws, payload, bits);
            } else if (stage == 1) {
                dvbs2_fast_bch(f, ws);
            } else if (stage == 2) {
                dvbs2_fast_ldpc(f, ws);
            } else {
                dvbs2_fast_map(f, ws, use ? ov : oc);
                dvbs2_fast_pl(f, use ? ov : oc);
            }
        }
        if (stage < 3 ? memcmp(c->bits, v->bits, length[stage]) != 0
                      : memcmp(oc, ov, symbols) != 0) {
            bad |= 1u << stage;
        }
        memcpy(v, c, sizeof *v);   // the next stage starts from the C result
    }
    return bad;
}

static void fecbench_task(void *waiter)
{
    const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t external = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    dvbs2_workspace *ws = heap_caps_aligned_alloc(16, sizeof *ws, external);
    uint8_t *payload = NULL;
    uint8_t *output = NULL;
    dvbs2_fast *fast = impl ? heap_caps_aligned_alloc(16, dvbs2_fast_size(), internal) : NULL;
    dvbs2_workspace *ws2 = impl >= 2 ? heap_caps_aligned_alloc(16, sizeof *ws2, external) : NULL;
    uint8_t *output2 = impl >= 2 ? heap_caps_malloc(DVBS2_MAX_SYMBOLS, external) : NULL;
    const unsigned options[] = {0, 0, 0, DVBS2_BCH_LEGACY | DVBS2_MAP_COMPACT,
                                DVBS2_BCH_NIBBLE, DVBS2_BCH_SRAM};
    const unsigned pie = impl >= 2 ? 15u | options[impl] : 0;
    if (!ws || (impl && !fast) || (impl >= 2 && (!ws2 || !output2))) {
        printf("@{\"cmd\":\"fecbench\",\"error\":\"encoder allocation failed\"}\n");
        goto done;
    }
    // The PIE mask actually used and the measured EE.LDXQ.32 offset (see dvbs2.h).
    unsigned pie_used = 0;
    int ldxq_bias = -1;
    if (fast) {
        if (dvbs2_fast_prepare(fast, &dvbs2_modes[0], 1, pie) != 0) {
            printf("@{\"cmd\":\"fecbench\",\"error\":\"prepare failed\"}\n");
            goto done;
        }
        pie_used = dvbs2_fast_pie(fast);
        ldxq_bias = dvbs2_fast_ldxq_bias(fast);
    }

    const esp_app_desc_t *app = esp_app_get_description();
    printf("@{\"cmd\":\"fecbench\",\"event\":\"begin\",\"firmware\":\"%s\",\"idf\":\"%s\","
           "\"start_us\":%" PRId64 ",\"cpu_mhz\":%d,\"core\":%d,\"modes\":%u,"
           "\"frames_per_mode\":%d,\"warmup_frames\":1,\"pilots\":true,\"pl_code\":0,"
           "\"rolloff\":0.35,\"inputs\":[\"gcs\",\"ts\"],\"workspace_bytes\":%u,"
           "\"payload_buffer_bytes\":%d,\"phase_buffer_bytes\":%d,\"impl\":\"%s\","
           "\"bch_variant\":\"%s\",\"fast_bytes\":%u,\"pie_mask\":%u,\"ldxq_bias\":%d,"
           "\"payload_memory\":\"internal\",\"workspace_memory\":\"psram\","
           "\"free_internal\":%u,\"largest_internal\":%u}\n",
           app->version, app->idf_ver, esp_timer_get_time(), CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
           xPortGetCoreID(), (unsigned)dvbs2_mode_count, FRAMES, (unsigned)sizeof *ws,
           DVBS2_FEC_BYTES, DVBS2_MAX_SYMBOLS, impl_names[impl >= 2 ? 2 : impl],
           impl_names[impl],
           fast ? (unsigned)dvbs2_fast_size() : 0, pie_used, ldxq_bias,
           (unsigned)heap_caps_get_free_size(internal),
           (unsigned)heap_caps_get_largest_free_block(internal));

    unsigned reported = 0;
    for (size_t m = 0; m < dvbs2_mode_count; ++m) {
        const dvbs2_mode *mode = &dvbs2_modes[m];
        const size_t payload_bytes = (mode->kbch - 80) / 8;
        const size_t symbols = dvbs2_frame_symbols(mode, PILOTS);
        heap_caps_free(output);
        heap_caps_free(fast);
        heap_caps_free(payload);
        const unsigned output_caps = mode->bits_per_symbol == 3 ? internal : external;
        // Largest allocation first. Cached workspace in PSRAM; hot scratch and
        // 8PSK phase output in SRAM. Payload fits the fragment after phases.
        output = heap_caps_malloc(symbols, output_caps);
        fast = impl ? heap_caps_aligned_alloc(16, dvbs2_fast_mode_size(mode), internal) : NULL;
        payload = heap_caps_malloc(payload_bytes, internal);
        if (!payload || !output || (impl && !fast)) {
            printf("@{\"cmd\":\"fecbench\",\"error\":\"mode allocation failed\"}\n");
            goto done;
        }
        printf("@{\"cmd\":\"fecbench\",\"event\":\"mode\",\"modcod\":%u,"
               "\"fast_bytes\":%u,\"output_memory\":\"%s\",\"free_internal\":%u,"
               "\"largest_internal\":%u}\n", mode->modcod,
               impl ? (unsigned)dvbs2_fast_mode_size(mode) : 0,
               output_caps == internal ? "internal" : "psram",
               (unsigned)heap_caps_get_free_size(internal),
               (unsigned)heap_caps_get_largest_free_block(internal));
        for (int input = 0; input < 2; ++input) {
            // For each mode/input, advance xorshift32 once per raw source byte and emit its low
            // byte. TS sync replacement still consumes that PRNG byte. Source and packet state
            // continue over timed frames. Warmup is discarded, then both states restart.
            const uint32_t seed = 2026u ^ ((uint32_t)mode->modcod << 16) ^ ((uint32_t)input << 8);
            uint32_t state = seed, cycles[STAGES], total;
            dvbs2_ts_state ts = {0};
            if (fast && (dvbs2_fast_prepare(fast, mode, PILOTS, pie) != 0 ||
                         dvbs2_fast_pie(fast) != pie_used)) {
                printf("@{\"cmd\":\"fecbench\",\"error\":\"prepare failed\","
                       "\"modcod\":%u}\n", mode->modcod);
                goto done;
            }
            fill_payload(payload, payload_bytes, input, 0, &state);
            if (impl >= 2 && input == 0) {
                const unsigned mismatch = check_pie(fast, mode, pie, ws2, ws, output2, output,
                                                    payload);
                printf("@{\"cmd\":\"fecbench\",\"event\":\"pie_check\",\"modcod\":%u,"
                       "\"mismatch\":%u}\n", mode->modcod, mismatch);
                if (dvbs2_fast_prepare(fast, mode, PILOTS, pie) != 0) {
                    goto done;
                }
            }
            if (encode(fast, ws, mode, input ? &ts : NULL, payload, output, cycles, &total) != 0) {
                printf("@{\"cmd\":\"fecbench\",\"error\":\"warmup encode failed\","
                       "\"modcod\":%u,\"input\":\"%s\"}\n", mode->modcod, input ? "ts" : "gcs");
                goto done;
            }
            state = seed;
            memset(&ts, 0, sizeof ts);
            for (unsigned frame = 0; frame < FRAMES; ++frame) {
                const size_t offset = frame * payload_bytes;
                fill_payload(payload, payload_bytes, input, offset, &state);
                if (encode(fast, ws, mode, input ? &ts : NULL, payload, output, cycles,
                           &total) != 0) {
                    printf("@{\"cmd\":\"fecbench\",\"error\":\"encode failed\","
                           "\"modcod\":%u,\"input\":\"%s\",\"frame\":%u}\n",
                           mode->modcod, input ? "ts" : "gcs", frame);
                    goto done;
                }
                const uint32_t crc = phase_crc32(output, symbols);
                printf("@{\"cmd\":\"fecbench\",\"event\":\"frame\",\"modcod\":%u,"
                       "\"impl\":\"%s\",\"rate\":\"%s\",\"modulation\":\"%s\","
                       "\"input\":\"%s\",\"frame\":%u,"
                       "\"seed\":%" PRIu32 ",\"source_offset\":%u,\"payload_bytes\":%u,"
                       "\"symbols\":%u,\"crc32\":%" PRIu32 ",\"cycles\":{"
                       "\"bb\":%" PRIu32 ",\"bb_scramble\":%" PRIu32 ",\"bch\":%" PRIu32 ","
                       "\"ldpc\":%" PRIu32 ",\"interleave_map\":%" PRIu32 ","
                       "\"pl\":%" PRIu32 ",\"total\":%" PRIu32 "}}\n",
                       mode->modcod, impl_names[impl >= 2 ? 2 : impl], mode->rate,
                       mode->bits_per_symbol == 3 ? "8psk" : "qpsk",
                       input ? "ts" : "gcs", frame, seed, (unsigned)offset,
                       (unsigned)payload_bytes, (unsigned)symbols, crc, cycles[0], cycles[1],
                       cycles[2], cycles[3], cycles[4], cycles[5], total);
                ++reported;
                vTaskDelay(1); // Let the idle task run between frames; never inside the timer.
            }
        }
    }
    printf("@{\"cmd\":\"fecbench\",\"event\":\"end\",\"frames_reported\":%u}\n", reported);

done:
    heap_caps_free(output2);
    heap_caps_free(ws2);
    heap_caps_free(fast);
    heap_caps_free(output);
    heap_caps_free(payload);
    heap_caps_free(ws);
    xTaskNotifyGive((TaskHandle_t)waiter);
    vTaskDelete(NULL);
}

int fecbench_run(const char *name)
{
    for (impl = 0; impl < sizeof impl_names / sizeof impl_names[0] &&
                   strcmp(name, impl_names[impl]) != 0; ++impl) {
    }
    if (impl == sizeof impl_names / sizeof impl_names[0]) {
        printf("@{\"cmd\":\"fecbench\",\"error\":\"implementation is plain, c, pie, legacy, nibble or sram\"}\n");
        return -1;
    }
    if (xTaskCreatePinnedToCore(fecbench_task, "fecbench", 4096,
                               xTaskGetCurrentTaskHandle(), tskIDLE_PRIORITY + 1,
                               NULL, 1) != pdPASS) {
        printf("@{\"cmd\":\"fecbench\",\"error\":\"task allocation failed\"}\n");
        return -1;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return 0;
}
