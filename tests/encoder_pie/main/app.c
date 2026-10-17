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
    unsigned cases = 0, quads = 0, resident = 0;
    uint8_t *quad = heap_caps_malloc(8352, caps);
    assert(quad);
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

            if (mc < 12) dvbs2_pack_quads(raw, quad, symbols);
            dvbs2_pack(raw, raw, symbols);
            for (unsigned sparse = 0; sparse < 2; ++sparse) {
                for (unsigned offset = 0; offset < 16; ++offset) {
                    const unsigned flags = sparse ? DVBS2_BCH_NIBBLE | DVBS2_MAP_SPARSE : 0;
                    assert(!dvbs2_fast_prepare(f, mode, pilots, 15 | flags));
                    if (!sparse && mode->t > 8) assert(!dvbs2_fast_cache_bch(f, cache));
                    memset(out, 0xa5, 16704);
                    uint8_t *tail = (uint8_t *)ws + DVBS2_FAST_WORKSPACE_USED;
                    const size_t tail_bytes = sizeof *ws - DVBS2_FAST_WORKSPACE_USED;
                    // The fill window may change while fast stages use bits[].
                    memset(tail, 13 * offset + mc, tail_bytes);
                    uint8_t *dst = out + 16 + offset;
                    assert(!dvbs2_encode_frame_packed(f, ws, NULL, payload, bytes * 8, dst));
                    assert(!memcmp(dst, raw, n));
                    for (size_t i = 0; i < tail_bytes; ++i)
                        assert(tail[i] == (uint8_t)(13 * offset + mc));
                    for (unsigned i = 0; i < 16 + offset; ++i) assert(out[i] == 0xa5);
                    for (unsigned i = 16 + offset + n; i < 16704; ++i) assert(out[i] == 0xa5);
                    ++cases;
                    if (mc < 12) {
                        assert(!dvbs2_fast_prepare(f, mode, pilots, 15 | flags | DVBS2_MAP_QUADS));
                        const size_t hot_bytes = dvbs2_fast_cache_size(f);
                        uint8_t *hot = heap_caps_aligned_alloc(16, hot_bytes+32, caps);
                        assert(hot && hot_bytes > 23000);
                        memset(hot,0xa5,hot_bytes+32);
                        assert(!dvbs2_fast_cache_tables(f,hot+16));
                        uint8_t *borrowed = dvbs2_fast_payload(f);
                        memcpy(borrowed, payload, bytes);
                        memset(out, 0xa5, 16704);
                        assert(!dvbs2_encode_frame_packed(f, ws, NULL, borrowed, bytes*8, dst));
                        const unsigned qn = (symbols+3)/4;
                        if (memcmp(dst, quad, qn)) {
                            for (unsigned i = 0; i < qn; ++i) if (dst[i] != quad[i]) {
                                printf("quad mismatch mc=%u pilots=%u sparse=%u offset=%u byte=%u got=%02x want=%02x\n",
                                       mc, pilots, sparse, offset, i, dst[i], quad[i]);
                                break;
                            }
                        }
                        assert(!memcmp(dst, quad, qn));
                        for (size_t i = 0; i < tail_bytes; ++i)
                            assert(tail[i] == (uint8_t)(13 * offset + mc));
                        for (unsigned i = 0; i < 16+offset; ++i) assert(out[i] == 0xa5);
                        for (unsigned i = 16+offset+qn; i < 16704; ++i) assert(out[i] == 0xa5);
                        for (unsigned i = 0; i < 16; ++i)
                            assert(hot[i] == 0xa5 && hot[16+hot_bytes+i] == 0xa5);
                        heap_caps_free(hot);
                        ++resident;
                        ++quads;
                    }
                }
            }
        }
    }
    printf("packed PIE: %u cases, direct/resident BCH and sparse/nibble BCH, all 14 modes, both pilots, all 16 alignments, guards intact\n", cases);
    printf("quad mapper: %u cases, borrowed payload, all alignments and guards intact\n", quads);
    printf("resident quad tables: %u cases, all rates/pilots/alignments, cache guards intact\n",resident);
    heap_caps_free(quad);
    heap_caps_free(cache); heap_caps_free(out); heap_caps_free(raw);
    heap_caps_free(payload); heap_caps_free(f); heap_caps_free(ws);
}

void app_main(void)
{
    puts("QEMU-START");
    assert(!fecbench_run("pie"));
    packed_check();
    void sign_mapper_check(void);
    sign_mapper_check();
    puts("QEMU-END");
    fflush(stdout);
    for (;;) vTaskDelay(1000);
}
