#include "rx_filter.h"
#include <limits.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_memory_utils.h"
#define RX_FILTER_DRAM DRAM_ATTR
#else
#define RX_FILTER_DRAM
#endif
#include "rx_filter_bank.h"

_Static_assert(RX_FILTER_PLANE >= 512 + 31 + 15 + 15, "physical vector padding");
_Static_assert(_Alignof(rx_filter) == 16, "state alignment");

static bool disjoint(const void *a, size_t na, const void *b, size_t nb)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return na <= UINTPTR_MAX-x && nb <= UINTPTR_MAX-y &&
           (x+na <= y || y+nb <= x);
}

static bool state_valid(const rx_filter *s)
{
    if (!s || ((uintptr_t)s & 15)) return false;
#ifdef ESP_PLATFORM
    if (!esp_ptr_internal(s) || !esp_ptr_internal((const char *)s+sizeof *s-1))
        return false;
#endif
    return true;
}

int16_t rx_filter_round(int64_t sum, unsigned shift)
{
    if (!shift || shift > 31) return 0;
    /* All callers' dot bounds are < 2^28, so the bias cannot overflow. */
    if (sum > INT64_MAX - (INT64_C(1) << (shift-1))) return INT16_MAX;
    int64_t biased = sum + (INT64_C(1) << (shift-1));
    int64_t unit = INT64_C(1) << shift;
    int64_t value = biased / unit - (biased < 0 && biased % unit != 0);
    return value < INT16_MIN ? INT16_MIN : value > INT16_MAX ? INT16_MAX : (int16_t)value;
}

static inline int16_t narrow(int32_t sum, unsigned shift)
{
    /* Dot bounds <2^28 permit fixed32-bit shifts in the hot path. */
    int32_t x = sum + (INT32_C(1) << (shift-1));
    int32_t unit = INT32_C(1) << shift;
    int32_t value = x >= 0 ? x >> shift : -((-x+unit-1) >> shift);
    return value < INT16_MIN ? INT16_MIN : value > INT16_MAX ? INT16_MAX : (int16_t)value;
}

bool rx_filter_init(rx_filter *s, uint64_t origin, uint32_t fraction, uint64_t step)
{
    if (!state_valid(s) || step < RX_FILTER_STEP_MIN || step > RX_FILTER_STEP_MAX ||
        origin > UINT64_MAX-8) return false;
    memset(s, 0, sizeof *s);
    s->origin = origin;
    s->fraction = fraction;
    s->step = step;
    return true;
}

bool rx_filter_set_step(rx_filter *s, uint64_t step)
{
    if (!state_valid(s) || s->failed || step < RX_FILTER_STEP_MIN || step > RX_FILTER_STEP_MAX)
        return false;
    s->step = step;
    return true;
}

#ifndef __XTENSA__
bool rx_filter_dot_s8(const int8_t *i, const int8_t *q, const int8_t *c, int32_t sums[2])
{
    int32_t a = 0, b = 0;
    for (unsigned k = 0; k < RX_FILTER_TAPS; ++k) {
        a += (int32_t)i[k]*c[k];
        b += (int32_t)q[k]*c[k];
    }
    sums[0] = a;
    sums[1] = b;
    return true;
}
#else
/* Source aligned32, destinations aligned16; complete groups of16 IQ pairs.
 * Leaf restores CPENABLE/PS; QR0/1 private. No look-behind or overread. */
extern bool rx_filter_unpack_s8(const int8_t *, int8_t *, int8_t *, unsigned);
#endif

static bool append(rx_filter *s, const int8_t *iq, size_t pairs, uint64_t next)
{
    uint64_t keep_start = next < s->received ? next : s->received;
    size_t keep = (size_t)(s->received - keep_start);
    if (keep > 31 || keep_start < s->base) return false;
    /* Move history before changing the alignment bias. These are only31B
     * per component; the incoming chunk never gets copied into a Q15 plane. */
    int8_t history[2][31];
    for (unsigned a = 0; a < 2; ++a)
        memcpy(history[a], s->plane[a]+s->bias+(size_t)(keep_start-s->base), keep);
    size_t prefix = pairs;
    if (!((uintptr_t)iq & 1)) {
        prefix = ((32-((uintptr_t)iq & 31)) & 31)/2;
        if (prefix > pairs) prefix = pairs;
    }
    unsigned bias = (unsigned)(-(keep+prefix)) & 15;
    for (unsigned a = 0; a < 2; ++a)
        memcpy(s->plane[a]+bias, history[a], keep);
    int8_t *i = s->plane[0]+bias+keep, *q = s->plane[1]+bias+keep;
    size_t at = 0;
    for (; at < prefix; ++at) { i[at] = iq[2*at]; q[at] = iq[2*at+1]; }
#ifdef __XTENSA__
    unsigned groups = (unsigned)((pairs-at)/16);
    if (groups && !rx_filter_unpack_s8(iq+2*at, i+at, q+at, groups)) return false;
    at += 16*groups;
#endif
    for (; at < pairs; ++at) { i[at] = iq[2*at]; q[at] = iq[2*at+1]; }
    /* Define every byte the aligned dot loads, without clearing a whole tile. */
    unsigned end = bias+(unsigned)(keep+pairs), padded = (end+15)&~15U;
    for (unsigned a = 0; a < 2; ++a) memset(s->plane[a]+end, 0, padded-end);
    s->base = keep_start;
    s->received += pairs;
    s->count = (uint16_t)(keep+pairs);
    s->bias = (uint16_t)bias;
    return true;
}

static uint64_t selection(const rx_filter *s, unsigned *phase)
{
    uint64_t b = ((uint64_t)s->fraction + (UINT64_C(1)<<25)) >> 26;
    *phase = (unsigned)b & 63;
    return s->origin + (b >> 6);
}

static bool emit(rx_filter *s, uint64_t at, unsigned phase,
                 int16_t pulse[2], int16_t derivative[2], bool want_derivative)
{
    if (at < s->base || at-s->base+32 > s->count) return false;
    size_t offset = s->bias+(size_t)(at-s->base);
    int32_t sums[2];
    if (!rx_filter_dot_s8(s->plane[0]+offset, s->plane[1]+offset,
                           rx_filter_pulse[phase], sums)) return false;
    for (unsigned a = 0; a < 2; ++a) {
        pulse[a] = narrow(sums[a], 1);
        if (derivative) {
            int32_t sum = 0;
            if (want_derivative)
                for (unsigned k = 0; k < 32; ++k)
                    sum += (int32_t)s->plane[a][offset+k]*rx_filter_derivative[phase][k];
            derivative[a] = narrow(sum, 9);
        }
    }
    uint64_t fraction = (uint64_t)s->fraction + (uint32_t)s->step;
    uint64_t advance = (s->step >> 32) + (fraction >> 32);
    if (s->origin > UINT64_MAX-advance-1) return false;
    s->origin += advance;
    s->fraction = (uint32_t)fraction;
    return true;
}

/* Sparse derivative work stays out of the common pulse loop's register set. */
static __attribute__((noinline)) void derivative_at(const rx_filter *s, unsigned offset,
                                                   unsigned phase, int16_t out[2])
{
    for (unsigned a = 0; a < 2; ++a) {
        int32_t sum = 0;
        for (unsigned k = 0; k < 32; ++k)
            sum += (int32_t)s->plane[a][offset+k]*rx_filter_derivative[phase][k];
        out[a] = narrow(sum, 9);
    }
}

/* Bounds and absolute64-bit headroom are established by push. At most64
 * outputs move a local origin by320 samples. A rounded history origin can
 * make relative == -1 initially; the rounded window itself is nonnegative.
 * Commit only completed outputs, including on a leaf-context refusal. */
static __attribute__((noinline)) bool emit_span(rx_filter *s, int16_t (*pulse)[2],
                    int16_t (*derivative)[2], size_t capacity,
                    uint32_t *mask_low, uint32_t *mask_high, rx_filter_progress *progress)
{
    int relative = s->origin < s->base ? -1 : (int)(s->origin-s->base);
    uint32_t fraction = s->fraction, step = (uint32_t)s->step;
    unsigned whole = (unsigned)(s->step >> 32), count = s->count, bias = s->bias;
    uint32_t low = *mask_low, high = *mask_high;
    size_t first = progress->written, j = first;
    bool ok = true;
    for (; j < capacity; ++j) {
        uint32_t rounded = fraction + (UINT32_C(1)<<25);
        unsigned at = (unsigned)(relative + (rounded < fraction));
        if (at+32 > count) break;
        unsigned offset = bias+at, phase = rounded >> 26;
        int32_t sums[2];
        if (!rx_filter_dot_s8(s->plane[0]+offset, s->plane[1]+offset,
                             rx_filter_pulse[phase], sums)) { ok = false; break; }
        pulse[j][0] = narrow(sums[0], 1);
        pulse[j][1] = narrow(sums[1], 1);
        if (derivative) {
            if (low & 1) derivative_at(s, offset, phase, derivative[j]);
            else derivative[j][0] = derivative[j][1] = 0;
        }
        uint32_t previous = fraction;
        fraction += step;
        relative += (int)(whole + (fraction < previous));
        low = (low >> 1) | (high << 31);
        high >>= 1;
    }
    if (j != first) {
        s->origin = s->base + (unsigned)relative;
        s->fraction = fraction;
    }
    *mask_low = low; *mask_high = high;
    progress->written = j;
    if (!ok) s->failed = true;
    return ok;
}

bool rx_filter_push(rx_filter *s, const int8_t *iq, size_t pairs,
                    int16_t (*pulse)[2], int16_t (*derivative)[2],
                    size_t capacity, uint64_t derivative_mask,
                    rx_filter_progress *progress)
{
    if (!state_valid(s) || s->failed || !pulse || !progress ||
        ((uintptr_t)pulse & 1) || ((uintptr_t)derivative & 1) ||
        ((uintptr_t)progress % _Alignof(rx_filter_progress)) ||
        !capacity || capacity > 64 || pairs > 512 || (pairs && !iq) ||
        (derivative_mask && !derivative) || (capacity < 64 && derivative_mask >> capacity) ||
        pairs > UINT64_MAX-s->received || s->step < RX_FILTER_STEP_MIN ||
        s->step > RX_FILTER_STEP_MAX || s->base > s->received ||
        s->received-s->base != s->count || s->count > 543 || s->bias > 15 ||
        s->origin == UINT64_MAX) return false;
    const void *p[5] = {s, iq, pulse, derivative, progress};
    size_t n[5] = {sizeof *s, 2*pairs, 4*capacity, derivative ? 4*capacity : 0, sizeof *progress};
    for (unsigned a = 0; a < 5; ++a)
        for (unsigned b = 0; b < a; ++b)
            if (n[a] && n[b] && !disjoint(p[a], n[a], p[b], n[b])) return false;
    *progress = (rx_filter_progress){0, 0};
    uint32_t mask_low = (uint32_t)derivative_mask, mask_high = (uint32_t)(derivative_mask >> 32);
    bool appended = false;
    while (progress->written < capacity) {
        unsigned phase;
        uint64_t at = selection(s, &phase);
        if (at <= s->received && s->received-at >= 32) {
            if (s->origin <= UINT64_MAX-321 && at >= s->base && at-s->base+32 <= s->count) {
                if (!emit_span(s, pulse, derivative, capacity, &mask_low, &mask_high, progress))
                    return false;
            } else {
                size_t j = progress->written;
                if (!emit(s, at, phase, pulse[j], derivative ? derivative[j] : NULL,
                          mask_low & 1)) { s->failed = true; return false; }
                ++progress->written;
                mask_low = (mask_low >> 1) | (mask_high << 31);
                mask_high >>= 1;
            }
        } else {
            if (appended || !pairs) break;
            if (!append(s, iq, pairs, at)) { s->failed = true; return false; }
            progress->consumed = pairs;
            appended = true;
        }
    }
    return true;
}

void rx_filter_preflight(rx_filter_check *result)
{
    if (!result) return;
    *result = (rx_filter_check){0};
#ifdef __XTENSA__
    result->native = true;
#endif
    _Alignas(16) int8_t i[64], q[64], c[32];
    for (unsigned offset = 0; offset < 16; ++offset) {
        for (unsigned pattern = 0; pattern < 4; ++pattern) {
            for (unsigned k = 0; k < 64; ++k) {
                i[k] = pattern == 0 ? -128 : pattern == 1 ? 127 : (int8_t)(k*73+offset);
                q[k] = pattern == 2 ? -128 : (int8_t)(k*17+pattern);
            }
            for (unsigned k = 0; k < 32; ++k)
                c[k] = pattern == 3 ? (k & 1 ? -128 : 127) : rx_filter_pulse[offset*4][k];
            int32_t want[2] = {0}, got[2] = {0};
            for (unsigned k = 0; k < 32; ++k) {
                want[0] += (int32_t)i[offset+k]*c[k];
                want[1] += (int32_t)q[offset+k]*c[k];
            }
            bool ok = rx_filter_dot_s8(i+offset, q+offset, c, got);
            ++result->cases;
            if (!ok || got[0] != want[0] || got[1] != want[1]) ++result->errors;
        }
    }
}
