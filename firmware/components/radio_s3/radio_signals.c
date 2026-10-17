// Independent diagnostic waveform generator. Equations and protocol also live in host/capture.py.
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

#define TAG_MASK 0x80200003u

static uint32_t pack(int i, int q)
{
    return ((uint32_t)i & 1023u) | (((uint32_t)q & 1023u) << 10);
}

static inline __attribute__((always_inline)) uint32_t step(uint32_t s)
{
    return (s >> 1) ^ ((0u - (s & 1u)) & TAG_MASK);
}

static IRAM_ATTR uint32_t apply(const uint32_t *matrix, uint32_t s)
{
    uint32_t out = 0;
    for (unsigned bit = 0; s; ++bit, s >>= 1) {
        if (s & 1u) {
            out ^= matrix[bit];
        }
    }
    return out;
}

static IRAM_ATTR uint32_t state_at(const radio_signal_t *signal, uint64_t n)
{
    // Max console duration is 60 s (< 107.374182375 s period), but arbitrary seeks also wrap.
    uint32_t index = n % UINT32_MAX, s = 1;
    for (unsigned bit = 0; index; ++bit, index >>= 1) {
        if (index & 1u) {
            s = apply(signal->jump[bit], s);
        }
    }
    return s;
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
    {"gap", 128, 128, 0, 0},    // +1 MHz in the chain's linear range; main.c sets radio_s3_set_gaps
};

bool radio_signal_init(radio_signal_t *signal, const char *name)
{
    signal->tag = strcmp(name, "tag") == 0;
    signal->sweep = strcmp(name, "sweep") == 0;
    signal->bandwidth_40m = signal->tag ? 40000000 : 2000000;
    if (signal->sweep) {
        // Only exposed at 80 MS/s: +/-1..20 MHz, 0.25 s each, amplitude 64.
        // Each step is a permutation of this 80-sample unit-frequency table.
        signal->bandwidth_40m = 20000000;
        for (unsigned n = 0; n < 80; ++n) {
            const double p = 6.2831853071795864769 * n / 80;
            signal->tone[n] = pack(lround(64 * cos(p)), lround(64 * sin(p)));
        }
        return true;
    }
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
    if (!signal->tag) {
        return false;
    }
    // Eight future output bits depend only on the low byte of the initial state. Each row
    // saves eight DAC words and the feedback term of an eight-step state transition.
    for (unsigned byte = 0; byte < 256; ++byte) {
        uint32_t s = byte;
        for (unsigned n = 0; n < 8; ++n) {
            signal->rows[byte][n] = pack((s & 1u) ? 256 : -256, 0);
            s = step(s);
        }
        signal->advance[byte] = s;
    }
    for (unsigned bit = 0; bit < 32; ++bit) {
        signal->jump[0][bit] = step(1u << bit);
    }
    for (unsigned k = 1; k < 32; ++k) {
        for (unsigned bit = 0; bit < 32; ++bit) {
            signal->jump[k][bit] = apply(signal->jump[k - 1], signal->jump[k - 1][bit]);
        }
    }
    return true;
}

static inline __attribute__((always_inline)) void copy8(uint32_t *dst, const uint32_t *src)
{
    // Word stores only: byte stores replicate across lanes (hardware-quirks.md:125-147
    // at the same pinned esp-sdr-zephyr revision cited above).
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
    if (signal->tag) {
        uint32_t s = state_at(signal, first);
        while (count >= 8) {
            const unsigned byte = s & 255u;
            copy8(dst, signal->rows[byte]);
            s = (s >> 8) ^ signal->advance[byte];
            dst += 8;
            count -= 8;
        }
        while (count--) {
            *dst++ = (s & 1u) ? 256u : 768u;
            s = step(s);
        }
    } else if (signal->sweep) {
        while (count) {
            const uint64_t dwell = 20000000; // 0.25 s at the required 80 MS/s
            const unsigned step = (first / dwell) % 40;
            const unsigned bin = step < 20 ? step + 1 : 80 - (step - 19);
            size_t take = dwell - first % dwell;
            if (take > count) take = count;
            uint32_t row[80];
            for (unsigned n = 0, phase = 0; n < 80; ++n) {
                row[n] = signal->tone[phase];
                phase += bin;
                if (phase >= 80) phase -= 80;
            }
            periodic(dst, first, take, row, 80);
            dst += take;
            first += take;
            count -= take;
        }
    } else {
        periodic(dst, first, count, signal->tone, 40);
    }
}
