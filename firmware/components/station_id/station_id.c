// Independent Morse and sample-index multiplexer. No radio or RTOS access.
#include "station_id.h"

#include <math.h>
#include <string.h>

#ifdef __XTENSA__
#include "esp_attr.h"
void station_repeat40(volatile uint32_t *dst, const uint32_t *row, unsigned periods);
#else
#define IRAM_ATTR
#endif

bool station_callsign(const char *in, char out[STATION_CALL_MAX + 1])
{
    if (!in) {
        return false;
    }
    size_t n = strlen(in);
    bool letter = false, digit = false, slash = true;
    if (n < 3 || n > STATION_CALL_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)in[i];
        if (c >= 'a' && c <= 'z') {
            c -= 'a' - 'A';
        }
        if (c >= 'A' && c <= 'Z') {
            letter = true;
        } else if (c >= '0' && c <= '9') {
            digit = true;
        } else if (c != '/' || slash || i == n - 1) {
            return false;
        }
        out[i] = (char)c;
        slash = c == '/';
    }
    out[n] = 0;
    return letter && digit;
}

bool station_id_init(station_id_t *id, const char *call, uint64_t samples,
                     station_fill_fn payload, void *ctx)
{
    return station_id_init_rate(id, call, samples, STATION_RATE, payload, ctx);
}

bool station_id_init_rate(station_id_t *id, const char *call, uint64_t samples, uint32_t rate,
                          station_fill_fn payload, void *ctx)
{
    // ITU-R M.1677-1 Annex 1, Part I, 1.1 pp. 2-3 (characters), 2 p. 3 (spacing).
    static const char *const code[] = {
        ".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---",
        "-.-", ".-..", "--", "-.", "---", ".--.", "--.-", ".-.", "...", "-",
        "..-", "...-", ".--", "-..-", "-.--", "--..",
        "-----", ".----", "..---", "...--", "....-", ".....", "-....", "--...", "---..", "----.",
        "-..-.",
    };
    char normalized[STATION_CALL_MAX + 1];
    if (!station_callsign(call, normalized) ||
            (rate != STATION_RATE && rate != 2 * STATION_RATE) || samples > 86400ULL * rate) {
        return false;
    }
    memset(id, 0, sizeof *id);
    id->scale = rate / STATION_RATE;
    const uint32_t dot = STATION_DOT * id->scale;
    uint32_t at = dot * 7; // silence to acquire the envelope floor
    for (const char *c = normalized; *c; ++c) {
        unsigned k = *c == '/' ? 36 : *c >= 'A' ? *c - 'A' : 26 + *c - '0';
        for (const char *m = code[k]; *m; ++m) {
            station_mark_t *mark = &id->marks[id->nmarks++];
            mark->start = at;
            at += dot * (*m == '-' ? 3 : 1);
            mark->end = at;
            at += dot;
        }
        at += dot * 2; // three dots total between characters
    }
    id->length = at + dot * 4; // seven dots total after last mark
    id->payload_samples = samples;
    id->payload = payload;
    id->ctx = ctx;
    // Repeat begins every 540 s of wall sample time. The initial and final IDs
    // are additional to the requested payload duration; no payload is dropped.
    uint64_t capacity = STATION_PERIOD * id->scale - id->length;
    uint64_t repeats = samples ? (samples - 1) / capacity : 0;
    id->total_samples = samples + (2 + repeats) * id->length;
    for (unsigned i = 0; i < 40; ++i) {
        double p = 6.2831853071795864769 * i / 40;
        id->cosine[i] = (int16_t)lround(32767 * cos(p));
        id->sine[i] = (int16_t)lround(32767 * sin(p));
    }
    for (unsigned i = 0; i <= 500; ++i) {
        id->edge[i] = (uint8_t)lround(64 * (1 - cos(3.14159265358979323846 * i / 500)));
    }
    return true;
}

static inline __attribute__((always_inline)) void copy8(volatile uint32_t *dst,
                                                          const uint32_t *src)
{
    // DAC SRAM needs word stores. Volatile also prevents compiler conversion
    // to memcpy/memset, which may use byte stores. See radio_signals.c:127-134.
    // Load groups before storing to hide the scalar load/use interlock.
    uint32_t a = src[0], b = src[1], c = src[2], d = src[3];
    dst[0] = a; dst[1] = b; dst[2] = c; dst[3] = d;
    a = src[4]; b = src[5]; c = src[6]; d = src[7];
    dst[4] = a; dst[5] = b; dst[6] = c; dst[7] = d;
}

static void IRAM_ATTR repeat(volatile uint32_t *dst, const uint32_t *row,
                             unsigned phase, size_t count)
{
    if (phase) {
        size_t n = 40 - phase < count ? 40 - phase : count;
        count -= n;
        while (n--) {
            *dst++ = row[phase++];
        }
    }
#ifdef __XTENSA__
    // Bank wrap and 400- or 800-sample envelope cells preserve 16-byte alignment.
    // Keep the scalar fallback for arbitrary seeks and partial test buffers.
    if (!((uintptr_t)dst & 15) && count >= 40) {
        size_t periods = count / 40;
        station_repeat40(dst, row, periods);
        dst += periods * 40;
        count -= periods * 40;
    }
#endif
    // A full period per iteration avoids two nested loops per eight words.
    while (count >= 40) {
        copy8(dst, row); copy8(dst + 8, row + 8); copy8(dst + 16, row + 16);
        copy8(dst + 24, row + 24); copy8(dst + 32, row + 32);
        dst += 40;
        count -= 40;
    }
    phase = 0;
    while (count--) {
        *dst++ = row[phase++];
    }
}

int IRAM_ATTR station_cw_fill(uint32_t *dst, uint64_t first, size_t count, void *ctx)
{
    const station_id_t *id = ctx;
    const uint32_t ramp = STATION_EDGE * id->scale, cell = STATION_CELL * id->scale;
    unsigned m = 0;
    int previous_amp = -1;
    uint32_t row[40] __attribute__((aligned(16)));
    while (count) {
        while (m < id->nmarks && id->marks[m].end <= first) {
            ++m;
        }
        size_t take = count;
        unsigned amp = 0;
        if (m < id->nmarks) {
            uint32_t start = id->marks[m].start, end = id->marks[m].end;
            uint64_t boundary;
            if (first < start) {
                boundary = start;
            } else if (first - start < ramp) {
                unsigned k = (first - start) / cell;
                amp = id->edge[k];
                boundary = start + (k + 1) * cell;
            } else if (end - first <= ramp) {
                unsigned k = (end - first - 1) / cell;
                amp = id->edge[k];
                boundary = end - k * cell;
            } else {
                amp = 128;
                boundary = end - ramp;
            }
            if (boundary - first < take) {
                take = boundary - first;
            }
        }
        if ((int)amp != previous_amp) {
            for (unsigned p = 0; p < 40; ++p) {
                int i = id->cosine[p] * (int)amp / 32767;
                int q = id->sine[p] * (int)amp / 32767;
                row[p] = ((uint32_t)i & 1023) | (((uint32_t)q & 1023) << 10);
            }
            previous_amp = (int)amp;
        }
        count -= take;
        unsigned phase = first % 40;
        first += take;
        repeat(dst, row, phase, take);
        dst += take;
    }
    return 0;
}

int IRAM_ATTR station_id_fill(uint32_t *dst, uint64_t first, size_t count, void *ctx)
{
    station_id_t *id = ctx;
    const uint64_t period = STATION_PERIOD * id->scale;
    const uint64_t capacity = period - id->length;
    const uint64_t final = id->total_samples - id->length;
    while (count) {
        uint64_t end, local;
        bool cw;
        if (first >= final) {
            cw = true;
            local = first - final;
            end = id->total_samples;
        } else {
            uint64_t cycle = first / period;
            local = first % period;
            cw = local < id->length;
            if (cw) {
                end = cycle * period + id->length;
            } else {
                end = (cycle + 1) * period;
                local = cycle * capacity + local - id->length;
            }
            if (end > final) {
                end = final;
            }
        }
        // Banks may look ahead beyond the last played sample. Pad with silence, through the
        // CW path's word copies: the one-word volatile loop took 5 cycles per sample, 79.7k
        // cycles for a nearly empty final bank (results/2026-10-16-buildout).
        if (first >= id->total_samples) {
            static uint32_t silence[40] __attribute__((aligned(16)));
            repeat(dst, silence, 0, count);
            return 0;
        }
        size_t take = count;
        if (end - first < take) {
            take = end - first;
        }
        int error = cw ? station_cw_fill(dst, local, take, id) :
                        id->payload(dst, local, take, id->ctx);
        if (error) {
            return error;
        }
        dst += take;
        first += take;
        count -= take;
    }
    return 0;
}

void station_payload_header(uint8_t *payload, size_t bytes, const char *call)
{
    if (!call[0] || bytes < STATION_HEADER_SIZE) {
        return;
    }
    memset(payload, ' ', STATION_HEADER_SIZE);
    memcpy(payload, "ESP-DVBS2 ID=", 13);
    memcpy(payload + 13, call, strlen(call));
    payload[STATION_HEADER_SIZE - 1] = '\n';
}
