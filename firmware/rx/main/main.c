// esp-dvbs2 receiver console. Results start with @; each command ends with done:true.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"

#include "capture.h"
#include "receive.h"
#include "replay.h"
#include "kernels.h"

// IDF's VFS drops characters after a 50 ms full-buffer timeout; block instead.
static int console_put(char c, FILE *stream)
{
    (void)stream;
    return usb_serial_jtag_write_bytes(&c, 1, portMAX_DELAY) == 1 ? 0 : EOF;
}

/* Logs from Wi-Fi start and stop would land inside rx's binary stream. */
static volatile bool quiet;
static int log_filter(const char *format, va_list args)
{
    return quiet ? 0 : vprintf(format, args);
}

static void print_capture(const capture_stats_t *s)
{
    printf(",\"samples\":%llu,\"dropped\":%llu,\"drops\":%lu,\"units\":%lu,\"max_late\":%lu,\"max_service\":%lu,"
           "\"max_service_at\":%llu,\"max_clear\":%lu,"
           "\"max_spin\":%lu,\"max_lag\":%lu,"
           "\"free_before\":%lu,\"free_wifi\":%lu,\"free_ready\":%lu,\"largest_ready\":%lu,"
           "\"min_free\":%lu,\"capture_stack_free\":%lu,"
           "\"gain_max\":%u,\"status\":\"%s\",\"cleanup\":\"%s\"",
           (unsigned long long)s->samples, (unsigned long long)s->dropped, (unsigned long)s->drops,
           (unsigned long)s->units, (unsigned long)s->max_late,
           (unsigned long)s->max_service, (unsigned long long)s->max_service_at,
           (unsigned long)s->max_clear,
           (unsigned long)s->max_spin, (unsigned long)s->max_lag,
           (unsigned long)s->free_before, (unsigned long)s->free_wifi,
           (unsigned long)s->free_ready, (unsigned long)s->largest_ready,
           (unsigned long)s->min_free, (unsigned long)s->stack_free, s->gain_max,
           esp_err_to_name(s->error), esp_err_to_name(s->cleanup));
    if (s->fault) printf(",\"fault\":\"%s\"", s->fault);
}

/* First n samples as separate I and Q planes in PSRAM, floor(s10/4), for host analysis. */
typedef struct { int8_t *i, *q; size_t n; } dump_t;

static esp_err_t IRAM_ATTR dump_unit(const uint32_t *w, size_t count, uint64_t first, void *ctx)
{
    dump_t *d = ctx;
    if (first < d->n) rx_unpack_span(w, first + count > d->n ? d->n - first : count,
                                     d->i + first, d->q + first);
    return ESP_OK;
}

static void dump(unsigned mhz, unsigned gain, unsigned pairs)
{
    dump_t d = {heap_caps_aligned_alloc(16, 2 * (size_t)pairs, MALLOC_CAP_SPIRAM), NULL, pairs};
    if (!d.i || !pairs) {
        printf("@{\"cmd\":\"dump\",\"error\":\"no memory for %u pairs\"}\n", pairs);
        return;
    }
    d.q = d.i + pairs;
    capture_config_t c = {.mhz = mhz, .gain = gain, .seconds = 1 + pairs / CAPTURE_RATE};
    capture_stats_t s;
    capture_run(&c, dump_unit, &d, &s);
    printf("@{\"cmd\":\"dump\",\"mhz\":%u,\"gain\":%u,\"pairs\":%u,\"layout\":\"i plane, q plane\"",
           mhz, gain, pairs);
    print_capture(&s);
    if (s.error == ESP_OK) printf(",\"binary_bytes\":%u", 2 * pairs);
    printf("}\n");
    if (s.error == ESP_OK) {
        fflush(stdout);
        receive_write(d.i, 2 * (size_t)pairs);
    }
    heap_caps_free(d.i);
}

/* Level, DC, ADC clipping and the gain field (bits 27:20) on every 64th sample. */
typedef struct {
    int64_t sum_i, sum_q, power;
    uint32_t n, clipped, sentinels, max_cycles;
    uint8_t gain_lo, gain_hi;
} probe_t;

static esp_err_t IRAM_ATTR probe_unit(const uint32_t *w, size_t count, uint64_t first, void *ctx)
{
    (void)first;
    probe_t *p = ctx;
    uint32_t start = esp_cpu_get_cycle_count();
    int32_t si = 0, sq = 0, clipped = 0, sentinels = 0;
    int64_t power = 0;
    uint8_t lo = p->gain_lo, hi = p->gain_hi;
    for (size_t k = 0; k < count; k += 64) {
        uint32_t x = w[k];
        int i = (int32_t)(x << 12) >> 22, q = (int32_t)(x << 22) >> 22;
        uint8_t g = x >> 20;
        si += i;
        sq += q;
        power += i * i + q * q;
        clipped += (i == -512) | (i == 511) | (q == -512) | (q == 511);
        sentinels += x == 0xa5a5a5a5U;
        lo = g < lo ? g : lo;
        hi = g > hi ? g : hi;
    }
    p->sum_i += si;
    p->sum_q += sq;
    p->power += power;
    p->n += (count + 63) / 64;
    p->clipped += clipped;
    p->sentinels += sentinels;
    p->gain_lo = lo;
    p->gain_hi = hi;
    uint32_t cycles = esp_cpu_get_cycle_count() - start;
    if (cycles > p->max_cycles) p->max_cycles = cycles;
    return ESP_OK;
}

static void rxprobe(const capture_config_t *c)
{
    probe_t p = {.gain_lo = 255};
    capture_stats_t s;
    capture_run(c, probe_unit, &p, &s);
    double n = p.n ? p.n : 1, di = p.sum_i / n, dq = p.sum_q / n;
    double rms = sqrt(p.power / n - di * di - dq * dq);
    printf("@{\"cmd\":\"rxprobe\",\"mhz\":%u,\"gain\":%u,\"seconds\":%u,"
           "\"rms\":%.2f,\"dc_i\":%.2f,\"dc_q\":%.2f,\"clipped\":%lu,\"sentinels\":%lu,"
           "\"gain_field\":[%u,%u],\"probe_cycles\":%lu",
           c->mhz, c->gain, c->seconds, rms, di, dq,
           (unsigned long)p.clipped, (unsigned long)p.sentinels, p.gain_lo, p.gain_hi,
           (unsigned long)p.max_cycles);
    print_capture(&s);
    printf("}\n");
}

/* "5m": 5 Msym/s; "" or "1m": 1 Msym/s; else 0. */
static unsigned symbol_rate(const char *token)
{
    return !*token || !strcmp(token, "1m") ? 1000000 : !strcmp(token, "5m") ? 5000000 : 0;
}

/* A receive or replay summary, left open for more fields. */
static void print_receive(const char *cmd, esp_err_t e, unsigned modcod, unsigned rate,
                          const receive_stats_t *r)
{
    printf("@{\"cmd\":\"%s\",\"result\":\"%s\",\"modcod\":%u,\"symbol_rate\":%u,\"frames\":%lu,"
           "\"good\":%lu,\"failed\":%lu,\"decoded\":%lu,\"frames_dropped\":%lu,\"max_iterations\":%lu,"
           "\"pushes\":%lu,\"max_push_cycles\":%lu,\"max_push_at\":%llu,\"push_cycles\":%llu,"
           "\"max_release_cycles\":%lu,\"release_cycles\":%llu,\"max_fec_cycles\":%lu,\"fec_cycles\":%llu,"
           "\"stack_free\":[%lu,%lu,%lu],\"free_internal\":%lu,\"largest_internal\":%lu,"
           "\"demod\":{\"words\":%llu,\"frames\":%lu,\"lost\":%lu,\"dropped\":%lu,\"relocks\":%lu,"
           "\"rejects\":%lu,\"locked\":%u,\"cfo_hz\":%ld,\"clock_ppb\":%ld,\"acquire_cycles\":%lu,"
           "\"dc\":[%d,%d]}",
           cmd, esp_err_to_name(e), modcod, rate, (unsigned long)r->frames, (unsigned long)r->good,
           (unsigned long)r->failed, (unsigned long)r->decoded, (unsigned long)r->dropped,
           (unsigned long)r->max_iterations, (unsigned long)r->pushes,
           (unsigned long)r->max_push_cycles, (unsigned long long)r->max_push_at,
           (unsigned long long)r->push_cycles, (unsigned long)r->max_release_cycles,
           (unsigned long long)r->release_cycles, (unsigned long)r->max_fec_cycles,
           (unsigned long long)r->fec_cycles, (unsigned long)r->release_stack_free,
           (unsigned long)r->check_stack_free, (unsigned long)r->decode_stack_free,
           (unsigned long)r->free_internal, (unsigned long)r->largest_internal,
           (unsigned long long)r->demod.words, (unsigned long)r->demod.frames,
           (unsigned long)r->demod.lost, (unsigned long)r->demod.dropped,
           (unsigned long)r->demod.relocks, (unsigned long)r->demod.rejects, r->demod.locked,
           (long)r->demod.cfo_hz, (long)r->demod.clock_ppb, (unsigned long)r->demod.acquire_cycles,
           r->demod.dc_i, r->demod.dc_q);
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
    esp_log_set_vprintf(log_filter);
    char line[96];
    while (true) {
        if (!fgets(line, sizeof line, stdin)) { vTaskDelay(1); continue; }
        line[strcspn(line, "\r\n")] = 0;
        char cmd[16], mode[8] = "";
        unsigned mhz, gain, seconds;
        int fields = sscanf(line, "%15s %u %u %u %7s", cmd, &mhz, &gain, &seconds, mode);
        if (!strcmp(line, "info")) {
            uint8_t mac[6];
            esp_read_mac(mac, ESP_MAC_WIFI_STA);
            printf("@{\"cmd\":\"info\",\"app\":\"esp_dvbs2_rx\",\"firmware\":\"%s\","
                   "\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"psram_bytes\":%u,"
                   "\"free_internal\":%u,\"largest_internal\":%u}\n",
                   esp_app_get_description()->version, mac[0], mac[1], mac[2], mac[3], mac[4],
                   mac[5], (unsigned)esp_psram_get_size(),
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        } else if (fields >= 4 && !strcmp(cmd, "rxprobe") && fields == 4) {
            capture_config_t c = {.mhz = mhz, .gain = gain, .seconds = seconds};
            rxprobe(&c);
        } else if (fields >= 5 && !strcmp(cmd, "rx")) {
            unsigned modcod = 0, rate = 1000000;
            char token[8] = "";
            if (sscanf(line, "rx %*u %*u %*u %u %7s", &modcod, token) == 2) rate = symbol_rate(token);
            capture_config_t c = {.mhz = mhz, .gain = gain, .seconds = seconds};
            static receive_stats_t r;
            capture_stats_t s = {0};
            quiet = true;
            esp_err_t e = rate ? receive_run(&c, modcod, rate, &r, &s) : ESP_ERR_INVALID_ARG;
            quiet = false;
            print_receive("rx", e, modcod, rate, &r);
            print_capture(&s);
            printf("}\n");
        } else if (fields == 2 && !strcmp(cmd, "upload")) {
            replay_upload(mhz);
        } else if (fields >= 2 && !strcmp(cmd, "replay")) {
            unsigned modcod = mhz, skip = 0, rate = 1000000;
            char *save, *token = strtok_r(line, " ", &save);
            strtok_r(NULL, " ", &save);
            while ((token = strtok_r(NULL, " ", &save)))    /* [skip] [5m] */
                strchr(token, 'm') ? (void)(rate = symbol_rate(token)) : (void)(skip = (unsigned)atoi(token));
            static receive_stats_t r;
            capture_stats_t s = {0};
            quiet = true;
            esp_err_t e = rate ? replay_run(modcod, rate, skip, &r, &s) : ESP_ERR_INVALID_ARG;
            quiet = false;
            print_receive("replay", e, modcod, rate, &r);
            print_capture(&s);
            printf(",\"push_words\":%u}\n@{\"cmd\":\"replay\",\"push_log\":[", CAPTURE_UNIT);
            for (unsigned k = 0; k < r.pushes && k < RECEIVE_LOG; ++k) printf("%s%lu", k ? "," : "", (unsigned long)r.log[k]);
            printf("]}\n");
        } else if (fields == 4 && !strcmp(cmd, "dump")) {
            dump(mhz, gain, seconds);
        } else if (fields >= 3 && !strcmp(cmd, "membench")) {
            capture_config_t c = {.mhz = mhz, .gain = gain, .seconds = 1};
            uint32_t r[16];
            capture_stats_t s;
            capture_bench(&c, r, &s);
            printf("@{\"cmd\":\"membench\",\"words\":4096,\"running\":{\"bank\":[%lu,%lu,%lu,%lu],"
                   "\"dram\":[%lu,%lu]},\"stopped\":{\"bank\":[%lu,%lu,%lu,%lu],\"dram\":[%lu,%lu]}",
                   r[0], r[1], r[2], r[3], r[4], r[5], r[8], r[9], r[10], r[11], r[12], r[13]);
            print_capture(&s);
            printf("}\n");
        } else {
            printf("@{\"error\":\"use info, rxprobe MHz gain seconds, rx MHz gain seconds MODCOD [5m], dump MHz gain pairs, upload bytes, replay MODCOD [skip] [5m], membench MHz gain\"}\n");
        }
        printf("@{\"done\":true}\n");
    }
}
