#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "console.h"
#include "dvbs2.h"
#include "rx_filter.h"
#include "rx_mc6.h"
#include "mc6_internal.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_app_desc.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "esp_private/esp_clk.h"
#include "freertos/task.h"

static _Alignas(16) rx_filter filter;
static int8_t input[2 * RX_FILTER_INPUT];
static int16_t pulse[RX_FILTER_OUTPUT][2], derivative[RX_FILTER_OUTPUT][2];
static dvbs2_workspace encoder;
static uint8_t payload[RX_MC6_PAYLOAD_BYTES], recovered[RX_MC6_PAYLOAD_BYTES];
static uint8_t word[RX_MC6_WORD_BYTES];

typedef struct { uint8_t *raw; size_t bytes; } buffer;

static buffer allocate(size_t bytes)
{
    buffer b = {heap_caps_aligned_alloc(16, bytes + 32,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT), bytes};
    if (b.raw) memset(b.raw, 0x6d, bytes + 32);
    return b;
}

static bool intact(buffer b)
{
    if (!b.raw) return false;
    for (unsigned n = 0; n < 16; ++n)
        if (b.raw[n] != 0x6d || b.raw[16 + b.bytes + n] != 0x6d) return false;
    return true;
}

static uint32_t random_word(uint32_t *state)
{
    *state ^= *state << 13; *state ^= *state >> 17; *state ^= *state << 5;
    return *state;
}

static bool fec_controls(void)
{
    uint32_t seed = 2026;
    const int16_t edge[] = {0, -32768, 32767, 1, -1, 3, -3, 169, 170};
    unsigned errors = 0;
    for (unsigned c = 0; c < 128; ++c) {
        _Alignas(16) rx_mc6_tile got = {0}, want = {0};
        for (unsigned n = 0; n < 4; ++n) got.before[n] = got.after[n] = 0x6d6d6d6d;
        for (unsigned e = 0; e < 10; ++e)
            for (unsigned lane = 0; lane < 8; ++lane)
                got.q[e][lane] = c < sizeof edge / sizeof *edge ? edge[c] :
                                  c == 9 ? (e < 2 ? (lane & 1 ? -8 : 8) : 2000) :
                                  (int16_t)random_word(&seed);
        want = got;
        for (unsigned lane = 0; lane < 8; ++lane) {
            unsigned low = 32767, high = 32767, index = 0, parity = 0, signs = 0;
            for (unsigned e = 0; e < 10; ++e) {
                int q = want.q[e][lane];
                unsigned a = q == -32768 ? 32767U : (unsigned)abs(q);
                if (a <= low) { high = low; low = a; index = e; }
                else if (a < high) high = a;
                parity ^= q < 0;
            }
            low = low * 3 / 4; high = high * 3 / 4;
            if (low > 127) low = 127;
            if (high > 127) high = 127;
            for (unsigned e = 0; e < 10; ++e) {
                bool negative = parity ^ (want.q[e][lane] < 0);
                int r = (int)(e == index ? high : low);
                int sum = want.q[e][lane] + (negative ? -r : r);
                if (sum < -32768) sum = -32768;
                if (sum > 32767) sum = 32767;
                want.q[e][lane] = (int16_t)sum;
                signs |= (unsigned)negative << e;
            }
            want.magnitudes[lane] = (uint16_t)(low | high << 8);
            want.metadata[lane] = (uint16_t)(signs | index << 10);
        }
        uint32_t ps, cp, sar, ps_after, cp_after, sar_after;
        __asm__ volatile("rsr.ps %0; rsr.cpenable %1; rsr.sar %2" : "=a"(ps), "=a"(cp), "=a"(sar));
        rx_mc6_leaf(&got);
        __asm__ volatile("rsr.ps %0; rsr.cpenable %1; rsr.sar %2" : "=a"(ps_after), "=a"(cp_after), "=a"(sar_after));
        if (memcmp(got.q, want.q, sizeof got.q) ||
            memcmp(got.magnitudes, want.magnitudes, sizeof got.magnitudes) ||
            memcmp(got.metadata, want.metadata, sizeof got.metadata) ||
            memcmp(got.before, want.before, sizeof got.before) ||
            memcmp(got.after, want.after, sizeof got.after) ||
            ps != ps_after || cp != cp_after || sar != sar_after) ++errors;
    }
    printf("@{\"cmd\":\"mc6_controls\",\"cases\":128,\"errors\":%u}\n", errors);
    return !errors;
}

static bool filter_frame(int8_t *in, int16_t (*pout)[2], int16_t (*dout)[2], bool external)
{
    if (!rx_filter_init(&filter, 0, 0, UINT64_C(4) << 32)) return false;
    unsigned outputs = 0, remaining = 4 * (33282 - 1) + 32;
    uint32_t seed = 2026, crc = 0, dcrc = 0;
    uint64_t cycles = 0;
    int64_t start = esp_timer_get_time(), wait = 0;
    do {
        unsigned pairs = remaining < RX_FILTER_INPUT ? remaining : RX_FILTER_INPUT;
        for (unsigned n = 0; n < 2 * pairs; ++n) in[n] = (int8_t)random_word(&seed);
        size_t used = 0;
        do {
            uint64_t mask = 0;
            for (unsigned j = 0; j < RX_FILTER_OUTPUT; ++j) {
                unsigned at = outputs + j;
                if (at < 90 || (at < 33282 && (at - 90) % 1476 >= 1440))
                    mask |= UINT64_C(1) << j;
            }
            rx_filter_progress p;
            uint32_t before = esp_cpu_get_cycle_count();
            bool ok = rx_filter_push(&filter, in + 2 * used, pairs - used,
                                    pout, dout, RX_FILTER_OUTPUT, mask, &p);
            cycles += (uint32_t)(esp_cpu_get_cycle_count() - before);
            if (!ok || p.consumed > pairs - used || p.written > RX_FILTER_OUTPUT) return false;
            used += p.consumed; outputs += p.written;
            crc = esp_rom_crc32_le(crc, (const uint8_t *)pout, 4 * p.written);
            dcrc = esp_rom_crc32_le(dcrc, (const uint8_t *)dout, 4 * p.written);
            if (!p.consumed && !p.written) break;
        } while (used < pairs || pairs == 0);
        if (used != pairs) return false;
        remaining -= pairs;
        int64_t before = esp_timer_get_time();
        vTaskDelay(1);
        wait += esp_timer_get_time() - before;
        if (!pairs) break;
    } while (true);
    printf("@{\"cmd\":\"filter_frame\",\"psram_io\":%s,\"outputs\":%u,\"push_elapsed_cycles\":%llu,"
           "\"elapsed_us\":%lld,\"wait_us\":%lld,\"pulse_crc32\":%lu,\"derivative_crc32\":%lu}\n",
           external ? "true" : "false", outputs, (unsigned long long)cycles, (long long)(esp_timer_get_time() - start),
           (long long)wait, (unsigned long)crc, (unsigned long)dcrc);
    /* Independent Python integer-dot oracle over this exact PRNG stream. */
    return outputs == 33282 && crc == UINT32_C(1678845495) && dcrc == UINT32_C(3371518343);
}

static bool filter_bench(void)
{
    rx_filter_check check;
    rx_filter_preflight(&check);
    printf("@{\"cmd\":\"filter_controls\",\"cases\":%lu,\"errors\":%lu,\"native\":%s}\n",
           (unsigned long)check.cases, (unsigned long)check.errors, check.native ? "true" : "false");
    if (check.errors || !check.native || !filter_frame(input, pulse, derivative, false)) return false;
    buffer a = allocate(sizeof input), b = allocate(sizeof pulse), c = allocate(sizeof derivative);
    bool ok = a.raw && b.raw && c.raw &&
              filter_frame((int8_t *)(a.raw + 16), (int16_t (*)[2])(b.raw + 16),
                           (int16_t (*)[2])(c.raw + 16), true) && intact(a) && intact(b) && intact(c);
    free(a.raw); free(b.raw); free(c.raw);
    return ok;
}

static bool fec_bench(void)
{
    if (!fec_controls()) return false;
    buffer beliefs = allocate(RX_MC6_N * sizeof(int16_t));
    buffer checks = allocate(RX_MC6_CHECKS * sizeof(rx_mc6_check));
    buffer channel = allocate(RX_MC6_N);
    bool ok = beliefs.raw && checks.raw && channel.raw;
    int8_t *llr = channel.raw ? (int8_t *)(channel.raw + 16) : NULL;
    const unsigned errors[] = {0, 9, 80};
    /* Packed Python encoder words, independent of the native encoder above. */
    const uint32_t crc_expected[] = {UINT32_C(4240430337), UINT32_C(2144775371), UINT32_C(1400211940)};
    for (unsigned c = 0; ok && c < 3; ++c) {
        for (unsigned n = 0; n < sizeof payload; ++n) payload[n] = (uint8_t)(17 * n + 31 * c + 3);
        const dvbs2_mode *mode = dvbs2_find_mode(6);
        ok = dvbs2_bb_gcs(&encoder, mode, payload, 8 * sizeof payload) == 0;
        if (!ok) break;
        dvbs2_bb_scramble(&encoder, mode);
        dvbs2_bch(&encoder, mode);
        dvbs2_ldpc(&encoder, mode);
        for (unsigned n = 0; n < RX_MC6_N; ++n)
            llr[n] = (encoder.bits[n / 8] & (0x80U >> (n % 8))) ? -24 : 24;
        for (unsigned n = 0; n < errors[c]; ++n) {
            unsigned at = (7919 * n + 137 * c) % RX_MC6_N;
            llr[at] = llr[at] < 0 ? 8 : -8;
        }
        rx_mc6 d = {0};
        uint32_t before = esp_cpu_get_cycle_count();
        ok = rx_mc6_init(&d, (int16_t *)(beliefs.raw + 16),
                         (rx_mc6_check *)(checks.raw + 16), llr);
        uint64_t cycles = (uint32_t)(esp_cpu_get_cycle_count() - before);
        uint64_t init_cycles = cycles, step_cycles = 0, check_cycles = 0;
        rx_mc6_result result = {0};
        unsigned tiles = 0;
        int64_t wait = 0;
        while (ok) {
            before = esp_cpu_get_cycle_count();
            ok = rx_mc6_pack(&d, word);
            bool decoded = ok && rx_mc6_check_word(word, &result);
            uint32_t elapsed = (uint32_t)(esp_cpu_get_cycle_count() - before);
            cycles += elapsed; check_cycles += elapsed;
            if (!ok || decoded || d.iteration == 10) break;
            unsigned target = d.iteration + 1;
            while (ok && d.iteration < target) {
                before = esp_cpu_get_cycle_count();
                unsigned budget = RX_MC6_TILES - tiles % RX_MC6_TILES;
                if (budget > 64) budget = 64;
                unsigned count = rx_mc6_step(&d, budget);
                elapsed = (uint32_t)(esp_cpu_get_cycle_count() - before);
                cycles += elapsed; step_cycles += elapsed;
                tiles += count;
                ok = count == budget && !d.failed;
                int64_t t = esp_timer_get_time();
                vTaskDelay(1);
                wait += esp_timer_get_time() - t;
            }
        }
        bool exact = ok && !memcmp(word, encoder.bits, sizeof word);
        size_t bytes = 0;
        before = esp_cpu_get_cycle_count();
        bool extracted = exact && rx_mc6_extract(word, recovered, sizeof recovered, &bytes);
        uint32_t extract_cycles = (uint32_t)(esp_cpu_get_cycle_count() - before);
        cycles += extract_cycles;
        bool data = extracted && bytes == sizeof payload && !memcmp(recovered, payload, bytes);
        printf("@{\"cmd\":\"mc6_frame\",\"case\":%u,\"flipped_bits\":%u,\"iterations\":%lu,"
               "\"tiles\":%u,\"work_elapsed_cycles\":%llu,\"wait_us\":%lld,"
               "\"init_cycles\":%llu,\"step_cycles\":%llu,\"check_cycles\":%llu,\"extract_cycles\":%lu,"
               "\"H\":%lu,\"bch\":%s,\"header\":%s,\"word_exact\":%s,\"payload_exact\":%s,"
               "\"word_crc32\":%lu,\"guards\":%s}\n",
               c, errors[c], (unsigned long)d.iteration, tiles, (unsigned long long)cycles,
               (long long)wait, (unsigned long long)init_cycles, (unsigned long long)step_cycles,
               (unsigned long long)check_cycles, (unsigned long)extract_cycles,
               (unsigned long)result.unsatisfied, result.bch_ok ? "true" : "false",
               result.header_ok ? "true" : "false", exact ? "true" : "false", data ? "true" : "false",
               (unsigned long)esp_rom_crc32_le(0, word, sizeof word),
               intact(beliefs) && intact(checks) && intact(channel) ? "true" : "false");
        ok = ok && data && intact(beliefs) && intact(checks) && intact(channel) &&
             esp_rom_crc32_le(0, word, sizeof word) == crc_expected[c];
    }
    free(beliefs.raw); free(checks.raw); free(channel.raw);
    return ok;
}

void app_main(void)
{
    usb_serial_jtag_driver_config_t usb = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb.rx_buffer_size = usb.tx_buffer_size = 8192;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
    usb_serial_jtag_vfs_use_driver();
    static FILE output = FDEV_SETUP_STREAM(console_put, NULL, NULL, _FDEV_SETUP_WRITE);
    stdout = &output;
    setvbuf(stdin, NULL, _IONBF, 0);
    char line[32];
    while (true) {
        if (!fgets(line, sizeof line, stdin)) { vTaskDelay(1); continue; }
        line[strcspn(line, "\r\n")] = 0;
        if (!strcmp(line, "info")) {
            uint8_t mac[6];
            ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
            printf("@{\"cmd\":\"info\",\"app\":\"rx_bench\",\"firmware\":\"%s\","
                   "\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"psram_bytes\":%u,"
                   "\"radio\":false,\"tx\":false,\"cpu_hz\":%u}\n",
                   esp_app_get_description()->version, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                   (unsigned)esp_psram_get_size(), (unsigned)esp_clk_cpu_freq());
        } else if (!strcmp(line, "filter") || !strcmp(line, "fec")) {
            bool ok = !strcmp(line, "filter") ? filter_bench() : fec_bench();
            unsigned reserve = (unsigned)uxTaskGetStackHighWaterMark(NULL);
            ok = ok && reserve >= 512;
            printf("@{\"cmd\":\"summary\",\"passed\":%s,\"radio\":false,\"tx\":false,"
                   "\"stack_free\":%u}\n", ok ? "true" : "false", reserve);
        } else printf("@{\"error\":\"use info, filter or fec\"}\n");
        printf("@{\"done\":true}\n");
    }
}
