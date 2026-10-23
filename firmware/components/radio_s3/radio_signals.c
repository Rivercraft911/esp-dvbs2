// Test tones. One DAC word holds signed I[9:0], Q[19:10].
// TX packing is I[9:0], Q[19:10], signed 10-bit. The task context reversed these fields:
// esp-sdr-zephyr 3214c16 doc/undocumented-registers-esp32s3.md:1354-1362 identifies TX vs RX.
// https://github.com/cyber-murmel/esp-sdr-zephyr/blob/3214c16a7c9f136414cc5ae9d245ea290c98a6ce/doc/undocumented-registers-esp32s3.md#L1354-L1362

#include "radio_signals.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef __XTENSA__
#include "esp_attr.h"
#else
#define IRAM_ATTR
#endif

static uint32_t pack(int i, int q)
{
    return ((uint32_t)i & 1023u) | (((uint32_t)q & 1023u) << 10);
}

// Periodic test words over 40 samples (1 MHz at 40 MS/s): I = di + ai cos p, Q = dq + aq sin p.
// Besides the +1 MHz tone, these separate the two DAC branches when a spectrum looks wrong.
static const struct {
    const char *name;
    int16_t ai, aq, di, dq;
} tones[] = {
    {"tone", 256, 256, 0, 0},   // +1 MHz
    {"neg", 256, -256, 0, 0},   // -1 MHz
    {"low", 64, 64, 0, 0},      // +1 MHz, 12 dB lower
    {"ionly", 256, 0, 0, 0},
    {"qonly", 0, 256, 0, 0},
    {"zero", 0, 0, 0, 0},       // carrier leak only
    {"dci", 0, 0, 200, 0},
    {"dcq", 0, 0, 0, 200},
};

bool radio_signal_init(radio_signal_t *signal, const char *name)
{
    signal->bandwidth_40m = 2000000;
    if (!strcmp(name, "two")) {
        signal->bandwidth_40m = 6000000;
        for (unsigned n = 0; n < 40; ++n) {
            // fs=40 MHz: +1 MHz has phase 2*pi*n/40. Two equal +1,+3 MHz tones.
            const double p = 6.2831853071795864769 * n / 40;
            signal->tone[n] = pack(lround(128 * (cos(p) + cos(3 * p))),
                                   lround(128 * (sin(p) + sin(3 * p))));
        }
        return true;
    }
    // "tone<amp>": the +1 MHz tone at amplitude 1..511, for drive-level sweeps.
    int ai = 0, aq = 0, di = 0, dq = 0;
    bool found = false;
    if (!strncmp(name, "tone", 4) && name[4] >= '1' && name[4] <= '9' && strlen(name) <= 7) {
        char *end;
        long amp = strtol(name + 4, &end, 10);
        found = !*end && amp <= 511;
        ai = aq = (int)amp;
    }
    for (size_t t = 0; !found && t < sizeof tones / sizeof tones[0]; ++t) {
        if (!strcmp(name, tones[t].name)) {
            found = true;
            ai = tones[t].ai, aq = tones[t].aq, di = tones[t].di, dq = tones[t].dq;
        }
    }
    if (found) {
        for (unsigned n = 0; n < 40; ++n) {
            const double p = 6.2831853071795864769 * n / 40;
            signal->tone[n] = pack(lround(di + ai * cos(p)), lround(dq + aq * sin(p)));
        }
        return true;
    }
    return false;
}

static inline __attribute__((always_inline)) void copy8(uint32_t *dst, const uint32_t *src)
{
    // Whole aligned words only: the DAC reads 32-bit sample words, and narrower stores into
    // a DAC bank have not been tested here, so no code relies on them.
    dst[0] = src[0], dst[1] = src[1], dst[2] = src[2], dst[3] = src[3];
    dst[4] = src[4], dst[5] = src[5], dst[6] = src[6], dst[7] = src[7];
}

static void IRAM_ATTR periodic(uint32_t *dst, uint64_t first, size_t count,
                                const uint32_t *tone, unsigned length)
{
    unsigned index = first % length;
    while (count) {
        size_t part = length - index;
        if (part > count) part = count;
        count -= part;
        const uint32_t *src = tone + index;
        while (part >= 8) {
            copy8(dst, src);
            dst += 8;
            src += 8;
            part -= 8;
        }
        while (part--) *dst++ = *src++;
        index = 0;
    }
}

void IRAM_ATTR radio_signal_fill(uint32_t *dst, uint64_t first, size_t count,
                                 radio_signal_t *signal)
{
    periodic(dst, first, count, signal->tone, 40);
}
