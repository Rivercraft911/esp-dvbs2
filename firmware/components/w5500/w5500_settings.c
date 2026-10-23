/* `eth` settings: defaults, console grammar and JSON (w5500.h). Portable C99. */
#include "w5500.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void w5500_settings_default(w5500_settings *s)
{
    /* Laptop 192.168.77.1; give each board its own address. The TX listens on 5004 and the RX
     * broadcasts to 5006, so a TX on the same switch never hears the RX's output. XIAO ESP32-S3
     * pins clear of the TX's SPI input preset (w5500_eth.h). */
    *s = (w5500_settings){
        .ip = {192, 168, 77, 2}, .mask = {255, 255, 255, 0}, .gateway = {0, 0, 0, 0},
        .dest = {255, 255, 255, 255}, .port = 5004, .dest_port = 5006, .sck_khz = 26667,
        .sclk = 9, .mosi = 43, .miso = 44, .cs = 3,
    };
}

/* GPIOs the W5500 may use on these boards (ESP32-S3 with octal PSRAM, console on USB):
 * 0-18, 21, 38-44, 47 and 48. Not 19-20 (USB D-/D+, the console), 22-25 (not bonded out,
 * ESP32-S3 TRM v1.8 p.478), 26-37 (SPI flash and octal PSRAM), nor the strapping pins 45-46
 * (TRM 8.1, p.534): GPIO45 is pulled down at reset to select 3.3 V flash, and the W5500's CS
 * pull-up there could select 1.8 V. */
static bool pin_ok(int p)
{
    return (p >= 0 && p <= 18) || p == 21 || (p >= 38 && p <= 44) || p == 47 || p == 48;
}

bool w5500_settings_valid(const w5500_settings *s)
{
    const int pin[4] = {s->sclk, s->mosi, s->miso, s->cs};
    for (unsigned i = 0; i < 4; ++i) {
        if (!pin_ok(pin[i])) return false;
        for (unsigned j = 0; j < i; ++j) if (pin[j] == pin[i]) return false;
    }
    return s->port && s->dest_port && s->sck_khz >= 10000 && s->sck_khz <= 40000;
}

static const char *skip(const char *p)
{
    while (*p == ' ' || *p == '\t') ++p;
    return p;
}

/* Copy one space-separated word; false if it does not fit. */
static bool word(const char **p, char *out, size_t cap)
{
    const char *q = skip(*p);
    size_t n = 0;
    while (q[n] && q[n] != ' ' && q[n] != '\t') ++n;
    if (!n || n >= cap) return false;
    memcpy(out, q, n);
    out[n] = 0;
    *p = q + n;
    return true;
}

static bool number(const char *t, unsigned long max, unsigned long *v)
{
    char *end;
    if (*t < '0' || *t > '9') return false;
    *v = strtoul(t, &end, 10);
    return !*end && *v <= max;
}

/* "A.B.C.D", then optionally ":PORT" or "/BITS" when tail is non-NULL. */
static bool address(const char *t, uint8_t ip[4], char sep, unsigned long max, unsigned long *tail)
{
    char part[16];
    for (unsigned i = 0; i < 4; ++i) {
        size_t n = 0;
        while (t[n] >= '0' && t[n] <= '9' && n < 3) ++n;
        unsigned long v;
        memcpy(part, t, n);
        part[n] = 0;
        if (!n || !number(part, 255, &v)) return false;
        ip[i] = (uint8_t)v;
        t += n;
        if (i < 3 && *t++ != '.') return false;
    }
    if (!*t) return !tail || sep == ':';
    return tail && *t == sep && number(t + 1, max, tail);
}

int w5500_settings_parse(w5500_settings *s, const char *args)
{
    w5500_settings n = *s;
    char key[12], a[24], b[24], c[24], extra[4];
    const char *p = args;
    unsigned long v;
    if (!word(&p, key, sizeof key)) return -1;
    if (!strcmp(key, "ip")) {
        unsigned long bits;
        if (!word(&p, a, sizeof a) || !address(a, n.ip, '/', 32, &bits) || bits < 1) return -1;
        const uint32_t m = 0xffffffffu << (32 - bits);
        for (unsigned i = 0; i < 4; ++i) n.mask[i] = (uint8_t)(m >> (24 - 8 * i));
        memset(n.gateway, 0, 4);
        if (word(&p, b, sizeof b)) {
            if (strcmp(b, "gw") || !word(&p, c, sizeof c) || !address(c, n.gateway, 0, 0, NULL))
                return -1;
        }
    } else if (!strcmp(key, "dest")) {
        unsigned long port = n.dest_port;
        if (!word(&p, a, sizeof a)) return -1;
        if (!strncmp(a, "broadcast", 9) && (!a[9] || a[9] == ':')) {
            memset(n.dest, 255, 4);
            if (a[9] && !number(a + 10, 65535, &port)) return -1;
        } else if (!address(a, n.dest, ':', 65535, &port)) {
            return -1;
        }
        if (!port) return -1;
        n.dest_port = (uint16_t)port;
    } else if (!strcmp(key, "port")) {
        if (!word(&p, a, sizeof a) || !number(a, 65535, &v) || !v) return -1;
        n.port = (uint16_t)v;
    } else if (!strcmp(key, "mhz")) {
        char *end;
        if (!word(&p, a, sizeof a)) return -1;
        const double mhz = strtod(a, &end);
        if (*end || !(mhz >= 10.0 && mhz <= 40.0)) return -1;
        n.sck_khz = (uint16_t)(mhz * 1000.0 + 0.5);
    } else if (!strcmp(key, "pins")) {
        int8_t *pin[4] = {&n.sclk, &n.mosi, &n.miso, &n.cs};
        for (unsigned i = 0; i < 4; ++i) {
            if (!word(&p, a, sizeof a) || !number(a, 48, &v)) return -1;
            *pin[i] = (int8_t)v;
        }
        if (!w5500_settings_valid(&n)) return -1;
    } else if (!strcmp(key, "defaults")) {
        w5500_settings_default(&n);
    } else {
        return -1;
    }
    if (word(&p, extra, sizeof extra) || *skip(p)) return -1;
    *s = n;
    return 1;
}

int w5500_settings_json(const w5500_settings *s, char *out, size_t cap)
{
#define IP(x) x[0], x[1], x[2], x[3]
    return snprintf(out, cap,
                    "\"ip\":\"%u.%u.%u.%u\",\"mask\":\"%u.%u.%u.%u\",\"gateway\":\"%u.%u.%u.%u\","
                    "\"port\":%u,\"dest\":\"%u.%u.%u.%u\",\"dest_port\":%u,\"sck_khz\":%u,"
                    "\"pins\":{\"sclk\":%d,\"mosi\":%d,\"miso\":%d,\"cs\":%d}",
                    IP(s->ip), IP(s->mask), IP(s->gateway), s->port, IP(s->dest), s->dest_port,
                    s->sck_khz, s->sclk, s->mosi, s->miso, s->cs);
#undef IP
}
