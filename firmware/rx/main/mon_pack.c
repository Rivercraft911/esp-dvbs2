// The monitor's datagram formats (mon_pack.h). Portable C99.
#include "mon_pack.h"

#include <stdio.h>
#include <string.h>

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

size_t mon_header(uint8_t *out, unsigned type, uint16_t seq, uint32_t t_us)
{
    memcpy(out, "PVMN", 4);
    out[4] = 1;
    out[5] = (uint8_t)type;
    put16(out + 6, seq);
    put32(out + 8, t_us);
    return MON_HEADER;
}

/* 0.01 dB as a decimal. */
static int cdb(char *out, size_t cap, int v)
{
    const char *sign = v < 0 ? "-" : "";
    const unsigned a = (unsigned)(v < 0 ? -v : v);
    return snprintf(out, cap, "%s%u.%02u", sign, a / 100, a % 100);
}

size_t mon_stats(uint8_t *out, uint16_t seq, uint32_t t_us, const mon_stats_t *s)
{
    char *j = (char *)out + mon_header(out, MON_STATS, seq, t_us);
    const size_t cap = MON_STATS_MAX;
    size_t n = 0;
#define ADD(...) (n += (size_t)snprintf(j + n, n < cap ? cap - n : 0, __VA_ARGS__))
    ADD("{\"run\":%lu,\"state\":\"%s\",\"freq_mhz\":%u,\"rate\":\"%s\",\"modcod\":%u,"
        "\"short\":true,\"gain\":%u,\"frames\":%lu,\"failed\":%lu,\"lost\":%lu,\"relocks\":%lu,"
        "\"dummies\":%lu,\"ldpc\":%lu,\"runs_dropped\":%lu,\"frames_dropped\":%lu",
        (unsigned long)s->run, s->track ? "track" : "acquire", s->freq_mhz,
        s->symbol_rate == 5000000 ? "5m" : "1m", s->modcod, s->gain, (unsigned long)s->frames,
        (unsigned long)s->failed, (unsigned long)s->lost, (unsigned long)s->relocks,
        (unsigned long)s->dummies, (unsigned long)s->ldpc, (unsigned long)s->runs_dropped,
        (unsigned long)s->frames_dropped);
    if (s->have_mer) {
        char a[16], b[16];
        cdb(a, sizeof a, s->mer_cdb);
        cdb(b, sizeof b, s->mer_med_cdb);
        const long ppm_milli = (long)s->clock_ppb;    /* ppb / 1000 = ppm, three decimals */
        ADD(",\"mer_db\":%s,\"mer_med_db\":%s,\"cfo_hz\":%ld,\"clock_ppm\":%s%ld.%03ld", a, b,
            (long)s->cfo_hz, ppm_milli < 0 ? "-" : "", (ppm_milli < 0 ? -ppm_milli : ppm_milli) / 1000,
            (ppm_milli < 0 ? -ppm_milli : ppm_milli) % 1000);
    }
    if (s->ts)
        ADD(",\"packets\":%lu,\"crc8\":%lu,\"cc\":%lu,\"tei\":%lu", (unsigned long)s->packets,
            (unsigned long)s->crc8, (unsigned long)s->cc, (unsigned long)s->tei);
    if (s->eth)
        ADD(",\"eth_sent\":%lu,\"eth_dropped\":%lu,\"eth_q_peak\":%lu",
            (unsigned long)s->eth_sent, (unsigned long)s->eth_dropped, (unsigned long)s->eth_q_peak);
    ADD(",\"mon_dropped\":%lu", (unsigned long)s->mon_dropped);
    if (s->core0_pmil >= 0) ADD(",\"core0_pct\":%d.%d", s->core0_pmil / 10, s->core0_pmil % 10);
    if (s->core1_pmil >= 0) ADD(",\"core1_pct\":%d.%d", s->core1_pmil / 10, s->core1_pmil % 10);
    ADD(",\"capture_drops\":%lu,\"heap_after_boot\":%lu}", (unsigned long)s->capture_drops,
        (unsigned long)s->heap_after_boot);
#undef ADD
    return MON_HEADER + (n < cap ? n : cap - 1);
}

size_t mon_samples(uint8_t *out, uint16_t seq, uint32_t t_us, uint16_t snapshot, unsigned chunk,
                   const int8_t *iq)
{
    uint8_t *p = out + mon_header(out, MON_SAMPLES, seq, t_us);
    put16(p, snapshot);
    p[2] = (uint8_t)chunk;
    p[3] = MON_CHUNKS;
    put32(p + 4, 16000000u);
    memcpy(p + 8, iq, 2u * MON_CHUNK);
    return MON_SAMPLES_BYTES;
}

size_t mon_samples_planes(uint8_t *out, uint16_t seq, uint32_t t_us, uint16_t snapshot,
                          unsigned chunk, const int8_t *i, const int8_t *q)
{
    uint8_t *p = out + mon_header(out, MON_SAMPLES, seq, t_us);
    put16(p, snapshot);
    p[2] = (uint8_t)chunk;
    p[3] = MON_CHUNKS;
    put32(p + 4, 16000000u);
    int8_t *iq = (int8_t *)(p + 8);
    i += chunk * MON_CHUNK;
    q += chunk * MON_CHUNK;
    for (unsigned k = 0; k < MON_CHUNK; ++k) {
        iq[2 * k] = i[k];
        iq[2 * k + 1] = q[k];
    }
    return MON_SAMPLES_BYTES;
}

size_t mon_symbols(uint8_t *out, uint16_t seq, uint32_t t_us, unsigned count, unsigned bits,
                   const int8_t *iq)
{
    if (count > MON_SYMBOLS_MAX) count = MON_SYMBOLS_MAX;
    uint8_t *p = out + mon_header(out, MON_SYMBOLS, seq, t_us);
    put16(p, (uint16_t)count);
    p[2] = (uint8_t)bits;
    p[3] = 0;
    memcpy(p + 4, iq, 2u * count);
    return MON_SYMBOLS_BYTES(count);
}

void mon_unpack(int8_t *iq, const uint32_t *words, size_t n)
{
    for (size_t k = 0; k < n; ++k) {
        const uint32_t w = words[k];
        iq[2 * k] = (int8_t)(((int32_t)(w << 12) >> 22) >> 2);
        iq[2 * k + 1] = (int8_t)(((int32_t)(w << 22) >> 22) >> 2);
    }
}

int16_t mon_median(int16_t *v, unsigned n)
{
    for (unsigned i = 1; i < n; ++i)        /* insertion sort: n is small */
        for (unsigned k = i; k > 0 && v[k - 1] > v[k]; --k) {
            const int16_t t = v[k];
            v[k] = v[k - 1];
            v[k - 1] = t;
        }
    return n ? v[n / 2] : 0;
}
