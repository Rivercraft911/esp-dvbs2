// Normal-frame encoder timing. The source and the phase-byte CRC are reproducible on the host.
// Only encoding is timed; source generation, CRC, printing and the between-frame yield are not.
// Interrupts stay enabled, so raw totals include interrupt service during a frame. A pinned task
// keeps every CCOUNT subtraction on the same CPU. This command never starts the radio.

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

static int encode(dvbs2_workspace *ws, const dvbs2_mode *mode, dvbs2_ts_state *ts,
                  const uint8_t *payload, uint8_t *output, uint32_t cycles[STAGES],
                  uint32_t *total)
{
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

static void fecbench_task(void *waiter)
{
    const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    dvbs2_workspace *ws = heap_caps_aligned_alloc(16, sizeof *ws, internal);
    uint8_t *payload = heap_caps_malloc(DVBS2_FEC_BYTES, internal);
    uint8_t *output = heap_caps_malloc(DVBS2_MAX_SYMBOLS, internal);
    if (!ws || !payload || !output) {
        printf("@{\"cmd\":\"fecbench\",\"error\":\"internal allocation failed\"}\n");
        goto done;
    }

    const esp_app_desc_t *app = esp_app_get_description();
    printf("@{\"cmd\":\"fecbench\",\"event\":\"begin\",\"firmware\":\"%s\",\"idf\":\"%s\","
           "\"start_us\":%" PRId64 ",\"cpu_mhz\":%d,\"core\":%d,\"modes\":%u,"
           "\"frames_per_mode\":%d,\"warmup_frames\":1,\"pilots\":true,\"pl_code\":0,"
           "\"rolloff\":0.35,\"inputs\":[\"gcs\",\"ts\"],\"workspace_bytes\":%u,"
           "\"payload_buffer_bytes\":%d,\"phase_buffer_bytes\":%d}\n",
           app->version, app->idf_ver, esp_timer_get_time(), CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
           xPortGetCoreID(), (unsigned)dvbs2_mode_count, FRAMES, (unsigned)sizeof *ws,
           DVBS2_FEC_BYTES, DVBS2_MAX_SYMBOLS);

    unsigned reported = 0;
    for (size_t m = 0; m < dvbs2_mode_count; ++m) {
        const dvbs2_mode *mode = &dvbs2_modes[m];
        const size_t payload_bytes = (mode->kbch - 80) / 8;
        const size_t symbols = dvbs2_frame_symbols(mode, PILOTS);
        for (int input = 0; input < 2; ++input) {
            // For each mode/input, advance xorshift32 once per raw source byte and emit its low
            // byte. TS sync replacement still consumes that PRNG byte. Source and packet state
            // continue over timed frames. Warmup is discarded, then both states restart.
            const uint32_t seed = 2026u ^ ((uint32_t)mode->modcod << 16) ^ ((uint32_t)input << 8);
            uint32_t state = seed, cycles[STAGES], total;
            dvbs2_ts_state ts = {0};
            fill_payload(payload, payload_bytes, input, 0, &state);
            if (encode(ws, mode, input ? &ts : NULL, payload, output, cycles, &total) != 0) {
                printf("@{\"cmd\":\"fecbench\",\"error\":\"warmup encode failed\","
                       "\"modcod\":%u,\"input\":\"%s\"}\n", mode->modcod, input ? "ts" : "gcs");
                goto done;
            }
            state = seed;
            memset(&ts, 0, sizeof ts);
            for (unsigned frame = 0; frame < FRAMES; ++frame) {
                const size_t offset = frame * payload_bytes;
                fill_payload(payload, payload_bytes, input, offset, &state);
                if (encode(ws, mode, input ? &ts : NULL, payload, output, cycles, &total) != 0) {
                    printf("@{\"cmd\":\"fecbench\",\"error\":\"encode failed\","
                           "\"modcod\":%u,\"input\":\"%s\",\"frame\":%u}\n",
                           mode->modcod, input ? "ts" : "gcs", frame);
                    goto done;
                }
                const uint32_t crc = phase_crc32(output, symbols);
                printf("@{\"cmd\":\"fecbench\",\"event\":\"frame\",\"modcod\":%u,"
                       "\"rate\":\"%s\",\"modulation\":\"%s\",\"input\":\"%s\",\"frame\":%u,"
                       "\"seed\":%" PRIu32 ",\"source_offset\":%u,\"payload_bytes\":%u,"
                       "\"symbols\":%u,\"crc32\":%" PRIu32 ",\"cycles\":{"
                       "\"bb\":%" PRIu32 ",\"bb_scramble\":%" PRIu32 ",\"bch\":%" PRIu32 ","
                       "\"ldpc\":%" PRIu32 ",\"interleave_map\":%" PRIu32 ","
                       "\"pl\":%" PRIu32 ",\"total\":%" PRIu32 "}}\n",
                       mode->modcod, mode->rate, mode->bits_per_symbol == 3 ? "8psk" : "qpsk",
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
    heap_caps_free(output);
    heap_caps_free(payload);
    heap_caps_free(ws);
    xTaskNotifyGive((TaskHandle_t)waiter);
    vTaskDelete(NULL);
}

void fecbench_run(void)
{
    if (xTaskCreatePinnedToCore(fecbench_task, "fecbench", 4096,
                               xTaskGetCurrentTaskHandle(), tskIDLE_PRIORITY + 1,
                               NULL, 1) != pdPASS) {
        printf("@{\"cmd\":\"fecbench\",\"error\":\"task allocation failed\"}\n");
        return;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}
