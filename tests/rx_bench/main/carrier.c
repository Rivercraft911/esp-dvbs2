/* Radio-off carrier controls and two independently generated MC6 frames. */
#include "rx_carrier.h"
#include "rx_mc6.h"
#include "dvbs2.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "esp_private/esp_clk.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct { uint8_t *raw; size_t bytes; } carrier_buffer;

static carrier_buffer carrier_allocate(size_t bytes, bool internal)
{
    unsigned caps = MALLOC_CAP_8BIT | (internal ? MALLOC_CAP_INTERNAL : MALLOC_CAP_SPIRAM);
    carrier_buffer b = {heap_caps_aligned_alloc(16, bytes+32, caps), bytes};
    if (b.raw) memset(b.raw, 0x6d, bytes+32);
    return b;
}

static bool carrier_guard(carrier_buffer b)
{
    if (!b.raw) return false;
    for (unsigned k = 0; k < 16; ++k)
        if (b.raw[k] != 0x6d || b.raw[16+b.bytes+k] != 0x6d) return false;
    return true;
}

static const char *carrier_bool(bool value) { return value ? "true" : "false"; }

static uint32_t carrier_fcr(void)
{
#ifdef __XTENSA__
    uint32_t value;
    __asm__ volatile("rur.fcr %0" : "=a"(value));
    return value;
#else
    return 0; /* Host controls simulate scheduling, never native FPU evidence. */
#endif
}

static int64_t carrier_yield(void)
{
    int64_t before = esp_timer_get_time();
    vTaskDelay(1);
    return esp_timer_get_time()-before;
}

static bool carrier_primitives(const uint8_t gold[RX_CARRIER_GOLD_BYTES])
{
    const int16_t samples[4][2] = {{-32768,-32768},{32767,-32768},{0,0},{-3,2}};
    uint32_t phasor_crc = 0, rotation_crc = 0;
    int64_t waits = 0;
    for (unsigned n = 0; n < 12288; ++n) {
        uint32_t phase = ((n/3)<<20)+(UINT32_C(1)<<19)+(n%3)-1;
        int16_t p[2], r[2];
        rx_carrier_phasor(phase, p);
        rx_carrier_rotate(samples[n%4], phase, r);
        phasor_crc = esp_rom_crc32_le(phasor_crc, (const uint8_t *)p, sizeof p);
        rotation_crc = esp_rom_crc32_le(rotation_crc, (const uint8_t *)r, sizeof r);
        if ((n&63U) == 63) waits += carrier_yield();
    }
    int8_t h4[90][2], h6[90][2];
    bool headers = rx_carrier_header(4, h4) && rx_carrier_header(6, h6);
    uint32_t gold_crc = esp_rom_crc32_le(0, gold, RX_CARRIER_GOLD_BYTES);
    uint32_t h4_crc = esp_rom_crc32_le(0, (const uint8_t *)h4, sizeof h4);
    uint32_t h6_crc = esp_rom_crc32_le(0, (const uint8_t *)h6, sizeof h6);
    bool ok = headers && phasor_crc == UINT32_C(3817551946) &&
              rotation_crc == UINT32_C(940406863) && gold_crc == UINT32_C(2505840001) &&
              h4_crc == UINT32_C(3422459321) && h6_crc == UINT32_C(1215837593);
    printf("@{\"cmd\":\"carrier_controls\",\"cases\":12288,\"phasor_crc32\":%lu,"
           "\"rotation_crc32\":%lu,\"gold_crc32\":%lu,\"header4_crc32\":%lu,"
           "\"header6_crc32\":%lu,\"wait_us\":%lld,\"passed\":%s}\n",
           (unsigned long)phasor_crc, (unsigned long)rotation_crc, (unsigned long)gold_crc,
           (unsigned long)h4_crc, (unsigned long)h6_crc, (long long)waits, carrier_bool(ok));
    return ok;
}

bool rx_carrier_bench(void)
{
    enum { FRAME, LLR, WORD, PAYLOAD, EXPECTED, PHASES, ENCODER, STATE, GOLD, BUFFERS };
    carrier_buffer b[BUFFERS] = {0};
    const size_t size[BUFFERS] = {RX_CARRIER_SYMBOLS*4U, RX_CARRIER_BITS,
        RX_CARRIER_WORD_BYTES, RX_MC6_PAYLOAD_BYTES, RX_MC6_PAYLOAD_BYTES,
        RX_CARRIER_SYMBOLS, sizeof(dvbs2_workspace), sizeof(rx_carrier), RX_CARRIER_GOLD_BYTES};
    bool ok = true;
    for (unsigned k = 0; k < BUFFERS; ++k) {
        b[k] = carrier_allocate(size[k], k == STATE || k == GOLD);
        if (!b[k].raw) { ok = false; break; }
    }
    if (!ok) goto done;
    int16_t (*frame)[2] = (int16_t (*)[2])(b[FRAME].raw+16);
    int8_t *llr = (int8_t *)(b[LLR].raw+16);
    uint8_t *word = b[WORD].raw+16, *payload = b[PAYLOAD].raw+16;
    uint8_t *expected = b[EXPECTED].raw+16, *phases = b[PHASES].raw+16;
    dvbs2_workspace *encoder = (dvbs2_workspace *)(b[ENCODER].raw+16);
    rx_carrier *state = (rx_carrier *)(b[STATE].raw+16);
    uint8_t *gold = b[GOLD].raw+16;
    bool state_internal = esp_ptr_internal(state) && esp_ptr_internal((uint8_t *)state+sizeof *state-1);
    bool gold_internal = esp_ptr_internal(gold) && esp_ptr_internal(gold+RX_CARRIER_GOLD_BYTES-1);
    bool psram = true;
    for (unsigned k = 0; k < STATE; ++k)
        psram = psram && esp_ptr_external_ram(b[k].raw+16) &&
                esp_ptr_external_ram(b[k].raw+16+b[k].bytes-1);
    uint32_t cpu = esp_clk_cpu_freq();
#ifdef __XTENSA__
    const bool native = true;
#else
    const bool native = false;
#endif
    uint32_t before = esp_cpu_get_cycle_count();
    rx_carrier_gold(gold);
    uint32_t gold_cycles = esp_cpu_get_cycle_count()-before;
    printf("@{\"cmd\":\"carrier_setup\",\"native\":%s,\"cpu_hz\":%lu,\"core\":%d,"
           "\"state_bytes\":%zu,\"gold_bytes\":%u,\"state_internal\":%s,\"gold_internal\":%s,"
           "\"PSRAM_IO\":%s,\"gold_init_cycles\":%lu}\n",
           carrier_bool(native), (unsigned long)cpu, xPortGetCoreID(), sizeof *state,
           RX_CARRIER_GOLD_BYTES, carrier_bool(state_internal), carrier_bool(gold_internal),
           carrier_bool(psram), (unsigned long)gold_cycles);
    ok = cpu == 240000000 && state_internal && gold_internal && psram && carrier_primitives(gold);
    const uint32_t expected_crc[2] = {UINT32_C(4240430337), UINT32_C(2144775371)};
    for (unsigned c = 0; ok && c < 2; ++c) {
        /* Only the transmitter sees truth here. Carrier/FEC receive samples. */
        for (unsigned n = 0; n < RX_MC6_PAYLOAD_BYTES; ++n)
            expected[n] = (uint8_t)(17*n+31*c+3);
        const dvbs2_mode *mode = dvbs2_find_mode(6);
        ok = mode && dvbs2_bb_gcs(encoder, mode, expected, 8*RX_MC6_PAYLOAD_BYTES) == 0;
        if (!ok) break;
        dvbs2_bb_scramble(encoder, mode); dvbs2_bch(encoder, mode); dvbs2_ldpc(encoder, mode);
        dvbs2_map(encoder, mode, phases+90);
        dvbs2_plframe(mode, 1, phases);
        const float pi = 3.14159265358979323846f;
        float cfo = c ? 12000.0f : -12000.0f, initial = c ? -0.4f : 1.4f;
        int64_t prepare_start = esp_timer_get_time(), prepare_wait = 0;
        for (unsigned n = 0; n < RX_CARRIER_SYMBOLS; ++n) {
            /* Independent native libm source, OUTSIDE all carrier timings.
             * No carrier table, fitted coefficient or expected receiver bit. */
            float theta = initial+2*pi*cfo*(float)n/1000000.0f+
                          0.08f*sinf(2*pi*(float)n/20000.0f)+phases[n]*(pi/4);
            float amplitude = 30.0f*1.4142135623730951f;
            frame[n][0] = (int16_t)lrintf(64*amplitude*cosf(theta));
            frame[n][1] = (int16_t)lrintf(64*amplitude*sinf(theta));
            if ((n&63U) == 63) prepare_wait += carrier_yield();
        }
        int64_t prepare_us = esp_timer_get_time()-prepare_start;
        uint32_t input_crc = esp_rom_crc32_le(0, (const uint8_t *)frame, b[FRAME].bytes);
        memset(state, 0, sizeof *state);
        memset(llr, 0x57, b[LLR].bytes); memset(word, 0xa5, b[WORD].bytes);
        memset(payload, 0x6d, b[PAYLOAD].bytes);
        rx_carrier_metadata metadata = {
            .first_native_sample = UINT64_C(0x123456789abcdef0)+UINT64_C(532512)*c,
            .input_origin = UINT64_C(0x10000000000)+UINT64_C(133128)*c,
            .input_step_q32 = (UINT64_C(4)<<32)+12345,
            .input_fraction = UINT32_C(0xf1234567), .symbol_rate_hz = 1000000, .modcod = 6
        };
        int64_t start = esp_timer_get_time(), waits = 0;
        uint32_t fcr_before = carrier_fcr();
        before = esp_cpu_get_cycle_count();
        bool estimated = fcr_before == 0 && rx_carrier_estimate(state, frame, NULL, gold, &metadata);
        uint32_t estimate_cycles = esp_cpu_get_cycle_count()-before;
        uint32_t fcr_estimated = carrier_fcr();
        uint64_t projection_cycles = 0;
        uint32_t max_chunk = 0;
        unsigned chunks = 0;
        ok = estimated && fcr_estimated == fcr_before;
        while (ok && state->cursor < RX_CARRIER_SYMBOLS) {
            before = esp_cpu_get_cycle_count();
            ok = rx_carrier_project(state, frame, gold, 64, llr, word);
            uint32_t elapsed = esp_cpu_get_cycle_count()-before;
            projection_cycles += elapsed;
            if (elapsed > max_chunk) max_chunk = elapsed;
            ++chunks;
            waits += carrier_yield();
        }
        uint32_t fcr_projected = carrier_fcr();
        bool complete = ok && state->cursor == RX_CARRIER_SYMBOLS && state->emitted == RX_CARRIER_BITS &&
                        fcr_projected == fcr_before;
        rx_mc6_result result = {0};
        size_t bytes = 0;
        uint32_t finish_cycles = 0;
        bool decoded = false;
        if (complete) {
            before = esp_cpu_get_cycle_count();
            decoded = rx_mc6_finish(word, payload, b[PAYLOAD].bytes, &bytes, &result, RX_MC6_BCH_BYTE);
            finish_cycles = esp_cpu_get_cycle_count()-before;
        }
        int64_t wall = esp_timer_get_time()-start;
        uint32_t fcr_after = carrier_fcr();
        /* Truth and CRC admission are exclusively after strict finish. */
        uint32_t word_crc = esp_rom_crc32_le(0, word, b[WORD].bytes);
        bool exact = decoded && bytes == RX_MC6_PAYLOAD_BYTES &&
                     !memcmp(word, encoder->bits, RX_MC6_WORD_BYTES) &&
                     !memcmp(payload, expected, RX_MC6_PAYLOAD_BYTES) && word_crc == expected_crc[c];
        bool signs = complete;
        if (complete)
            for (unsigned bit = 0; bit < RX_CARRIER_BITS; ++bit)
                signs = signs && (((word[bit/8]>>(7-(bit&7U)))&1U) == (llr[bit] < 0));
        bool immutable = input_crc == esp_rom_crc32_le(0, (const uint8_t *)frame, b[FRAME].bytes) &&
                         esp_rom_crc32_le(0, gold, RX_CARRIER_GOLD_BYTES) == UINT32_C(2505840001);
        bool guards = true;
        for (unsigned k = 0; k < BUFFERS; ++k) guards = guards && carrier_guard(b[k]);
        bool metadata_exact = estimated && state->metadata.first_native_sample == metadata.first_native_sample &&
            state->metadata.input_origin == metadata.input_origin && state->metadata.input_fraction == metadata.input_fraction &&
            state->metadata.input_step_q32 == metadata.input_step_q32 && state->metadata.modcod == 6 &&
            state->metadata.symbol_rate_hz == metadata.symbol_rate_hz;
        char h[16]; snprintf(h, sizeof h, "%lu", (unsigned long)result.unsatisfied);
        printf("@{\"cmd\":\"carrier_frame\",\"case\":%u,\"modcod\":6,\"estimated\":%s,\"complete\":%s,"
               "\"estimate_cycles\":%lu,\"projection_cycles\":%llu,\"finish_cycles\":%lu,"
               "\"work_elapsed_cycles\":%llu,\"max_project_chunk_cycles\":%lu,\"project_chunks\":%u,"
               "\"elapsed_us\":%lld,\"wait_us\":%lld,\"prepare_us\":%lld,\"prepare_wait_us\":%lld,"
               "\"input_crc32\":%lu,\"word_crc32\":%lu,\"decoded\":%s,\"H\":%s,\"BCH\":%s,\"header\":%s,"
               "\"payload_bytes\":%zu,\"exact\":%s,\"signs_match\":%s,\"guards\":%s,\"input_immutable\":%s,"
               "\"metadata_exact\":%s,\"first_native_sample\":%llu,\"input_origin\":%llu,"
               "\"input_fraction\":%lu,\"input_step_q32\":%llu,\"FCR_observed\":%s,"
               "\"FCR_before\":%lu,\"FCR_estimated\":%lu,\"FCR_projected\":%lu,\"FCR_after\":%lu,"
               "\"cpu_after_hz\":%lu,\"stack_min_free\":%u}\n",
               c, carrier_bool(estimated), carrier_bool(complete), (unsigned long)estimate_cycles,
               (unsigned long long)projection_cycles, (unsigned long)finish_cycles,
               (unsigned long long)(estimate_cycles+projection_cycles+finish_cycles),
               (unsigned long)max_chunk, chunks, (long long)wall, (long long)waits,
               (long long)prepare_us, (long long)prepare_wait,
               (unsigned long)input_crc, (unsigned long)word_crc, carrier_bool(decoded),
               complete ? h : "null", complete ? carrier_bool(result.bch_ok) : "null",
               complete ? carrier_bool(result.header_ok) : "null", bytes, carrier_bool(exact),
               carrier_bool(signs), carrier_bool(guards), carrier_bool(immutable), carrier_bool(metadata_exact),
               (unsigned long long)metadata.first_native_sample, (unsigned long long)metadata.input_origin,
               (unsigned long)metadata.input_fraction, (unsigned long long)metadata.input_step_q32,
               carrier_bool(native), (unsigned long)fcr_before, (unsigned long)fcr_estimated,
               (unsigned long)fcr_projected, (unsigned long)fcr_after,
               (unsigned long)esp_clk_cpu_freq(), (unsigned)uxTaskGetStackHighWaterMark(NULL));
        ok = complete && exact && signs && guards && immutable && metadata_exact &&
             fcr_after == fcr_before && (uint32_t)esp_clk_cpu_freq() == cpu;
    }
done:
    for (unsigned k = 0; k < BUFFERS; ++k) {
        if (b[k].raw && !carrier_guard(b[k])) ok = false;
        free(b[k].raw);
    }
    printf("@{\"cmd\":\"carrier_done\",\"passed\":%s,\"continuous_qualified\":false}\n", carrier_bool(ok));
    return ok;
}
