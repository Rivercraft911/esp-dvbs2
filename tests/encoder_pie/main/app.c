// Correctness only. No radio, no peripherals, no useful timing from QEMU.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "dvbs2.h"
#include "fecbench.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void packed_check(void)
{
    const unsigned caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    dvbs2_workspace *ws = heap_caps_aligned_alloc(16, sizeof *ws, caps);
    dvbs2_fast *f = heap_caps_aligned_alloc(16, dvbs2_fast_size(), caps);
    uint8_t *payload = heap_caps_malloc(8100, caps);
    uint8_t *raw = heap_caps_malloc(DVBS2_MAX_SYMBOLS, caps);
    uint8_t *out = heap_caps_aligned_alloc(16, 16704, caps);
    uint8_t *cache = heap_caps_aligned_alloc(16, DVBS2_BCH_CACHE_BYTES, caps);
    assert(ws && f && payload && raw && out && cache);
    unsigned cases = 0;
    for (unsigned mc = 4; mc <= 17; ++mc) {
        const dvbs2_mode *mode = dvbs2_find_mode(mc);
        const unsigned bytes = (mode->kbch - 80) / 8;
        uint32_t x = 2026 ^ mc;
        for (unsigned i = 0; i < bytes; ++i) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            payload[i] = (uint8_t)x;
        }
        for (unsigned pilots = 0; pilots < 2; ++pilots) {
            const unsigned symbols = dvbs2_frame_symbols(mode, pilots), n = symbols / 2;
            assert(!dvbs2_fast_prepare(f, mode, pilots, 0));
            assert(!dvbs2_encode_frame(f, ws, NULL, payload, bytes * 8, raw));
            dvbs2_pack(raw, raw, symbols);
            for (unsigned offset = 0; offset < 16; ++offset) {
                assert(!dvbs2_fast_prepare(f, mode, pilots, 15));
                if (mode->t > 8) assert(!dvbs2_fast_cache_bch(f, cache));
                memset(out, 0xa5, 16704);
                uint8_t *dst = out + 16 + offset;
                assert(!dvbs2_encode_frame_packed(f, ws, NULL, payload, bytes * 8, dst));
                assert(!memcmp(dst, raw, n));
                for (unsigned i = 0; i < 16 + offset; ++i) assert(out[i] == 0xa5);
                for (unsigned i = 16 + offset + n; i < 16704; ++i) assert(out[i] == 0xa5);
                ++cases;
            }
        }
    }
    printf("packed PIE: %u cases, all 14 modes, both pilots, all 16 alignments, resident BCH, guards intact\n", cases);
    heap_caps_free(cache); heap_caps_free(out); heap_caps_free(raw);
    heap_caps_free(payload); heap_caps_free(f); heap_caps_free(ws);
}

void app_main(void)
{
    puts("QEMU-START");
    assert(!fecbench_run("pie"));
    packed_check();
    puts("QEMU-END");
    fflush(stdout);
    for (;;) vTaskDelay(1000);
}
