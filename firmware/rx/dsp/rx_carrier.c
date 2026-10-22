#include "rx_carrier.h"
#include <math.h>
#include <string.h>
#include "rx_carrier_table.h"

#define PI 3.14159265358979323846f
#define ROOT_HALF 0.7071067811865475244f

typedef struct { float r, i; } complex;

static bool disjoint(const void *a, size_t n, const void *b, size_t m)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return n <= UINTPTR_MAX-x && m <= UINTPTR_MAX-y && (x+n <= y || y+m <= x);
}

static void gold_step(uint32_t *x, uint32_t *y)
{
    *x = (*x >> 1) | (((*x ^ (*x >> 7)) & 1U) << 17);
    *y = (*y >> 1) | (((*y ^ (*y >> 5) ^ (*y >> 7) ^ (*y >> 10)) & 1U) << 17);
}

void rx_carrier_gold(uint8_t out[RX_CARRIER_GOLD_BYTES])
{
    if (!out) return;
    uint32_t x = 1, y = 262143, xh = 4104, yh = 195240;
    for (unsigned byte = 0; byte < RX_CARRIER_GOLD_BYTES; ++byte) {
        unsigned packed = 0;
        for (unsigned k = 0; k < 4; ++k) {
            unsigned r = ((x^y)&1U) | (((xh^yh)&1U)<<1);
            packed |= r << (2*k);
            gold_step(&x, &y); gold_step(&xh, &yh);
        }
        out[byte] = (uint8_t)packed;
    }
}

static unsigned quarter(const uint8_t *gold, unsigned n)
{
    unsigned k = n-90;
    return (gold[k/4] >> (2*(k%4))) & 3U;
}

bool rx_carrier_header(unsigned modcod, int8_t signs[90][2])
{
    if (!signs || (modcod != 4 && modcod != 6)) return false;
    const uint32_t rows[5] = {0x55555555, 0x33333333, 0x0f0f0f0f, 0x00ff00ff, 0x0000ffff};
    uint32_t y = 0;
    for (unsigned k = 0; k < 5; ++k)
        if ((modcod >> (4-k)) & 1U) y ^= rows[k];
    for (unsigned n = 0; n < 90; ++n) {
        unsigned bit;
        if (n < 26) bit = (UINT32_C(0x18d2e82) >> (25-n)) & 1U;
        else {
            unsigned k = n-26;
            bit = ((y >> (31-k/2)) & 1U) ^ (k&1U);
            bit ^= (unsigned)(UINT64_C(0x719d83c953422dfa) >> (63-k)) & 1U;
        }
        signs[n][0] = (bit ^ (n&1U)) ? -1 : 1;
        signs[n][1] = bit ? -1 : 1;
    }
    return true;
}

static int16_t sine(unsigned phase)
{
    unsigned quadrant = phase >> 10, k = phase & 1023U;
    int16_t v = carrier_sine[quadrant & 1U ? 1024-k : k];
    return quadrant & 2U ? (int16_t)-v : v;
}

void rx_carrier_phasor(uint32_t phase, int16_t out[2])
{
    unsigned p = ((phase + (UINT32_C(1)<<19)) >> 20) & 4095U;
    out[0] = sine((p+1024)&4095U);
    out[1] = sine(p);
}

static int16_t floor16(int32_t x)
{
    return (int16_t)(x >= 0 ? x/65536 : -((-x+65535)/65536));
}

void rx_carrier_rotate(const int16_t sample[2], uint32_t phase, int16_t out[2])
{
    int16_t phasor[2];
    rx_carrier_phasor(phase, phasor);
    int32_t i = sample[0], q = sample[1], c = phasor[0], s = phasor[1];
    out[0] = floor16(i*c-q*s);
    out[1] = floor16(i*s+q*c);
}

/* Used only while preparing the sparse pilot model, never per data symbol. */
static uint32_t phase_word(double radians)
{
    double turns = radians / 6.283185307179586476925286766559;
    turns -= floor(turns);
    uint64_t rounded = (uint64_t)floor(turns*4294967296.0 + .5);
    return (uint32_t)rounded;
}

static float unwrap(float a, float previous)
{
    while (a-previous > PI) a -= 2*PI;
    while (a-previous < -PI) a += 2*PI;
    return a;
}

static complex product(complex a, complex b)
{
    return (complex){a.r*b.r-a.i*b.i, a.r*b.i+a.i*b.r};
}

static unsigned anchor_twice(unsigned n) { return n ? 3095+2952*(n-1) : 89; }
static float anchor(unsigned n) { return anchor_twice(n)*.5f; }

/* Rotation remains unrounded until the block sums: Q6*Q15 /2^21. */
static complex known(const int16_t sample[2], uint32_t phase)
{
    int16_t p[2]; rx_carrier_phasor(phase, p);
    int32_t i = sample[0], q = sample[1];
    return (complex){(i*p[0]-q*p[1])/2097152.0f, (i*p[1]+q*p[0])/2097152.0f};
}

/* Strict 90-degree wedge: diagonal ties are inside. Signed zero and
 * nonfinite inputs retain the old diagnostic convention. */
static bool pilot_error(float r, float i)
{
    if (!isfinite(r) || !isfinite(i)) return fabsf(atan2f(i, r)) > PI/4;
    if (r == 0 && i == 0) return signbit(r) != 0;
    return r < 0 || fabsf(i) > r;
}

bool rx_carrier_estimate(rx_carrier *state, const int16_t frame[RX_CARRIER_SYMBOLS][2],
                        const int16_t derivative[RX_CARRIER_KNOWN][2],
                        const uint8_t gold[RX_CARRIER_GOLD_BYTES],
                        const rx_carrier_metadata *metadata)
{
    if (!state || !frame || !gold || !metadata ||
        ((uintptr_t)state % _Alignof(rx_carrier)) ||
        ((uintptr_t)metadata % _Alignof(rx_carrier_metadata)) || ((uintptr_t)frame & 1U) ||
        ((uintptr_t)derivative & 1U) || (metadata->modcod != 4 && metadata->modcod != 6) ||
        !isfinite(metadata->symbol_rate_hz) || metadata->symbol_rate_hz <= 0 ||
        !disjoint(state, sizeof *state, frame, RX_CARRIER_SYMBOLS*4U) ||
        !disjoint(state, sizeof *state, gold, RX_CARRIER_GOLD_BYTES) ||
        !disjoint(state, sizeof *state, metadata, sizeof *metadata) ||
        (derivative && !disjoint(state, sizeof *state, derivative, RX_CARRIER_KNOWN*4U))) return false;
    rx_carrier next = {.metadata = *metadata};
    int8_t header[90][2];
    if (!rx_carrier_header(metadata->modcod, header)) return false;
    float previous = 0;
    for (unsigned n = 0; n < 90; ++n) {
        complex x = {frame[n][0]/64.0f, frame[n][1]/64.0f};
        complex reference = {ROOT_HALF*header[n][0], -ROOT_HALF*header[n][1]};
        complex v = product(x, reference);
        float a = atan2f(v.i, v.r);
        if (n) a = unwrap(a, previous);
        previous = a;
        next.slope += (n-44.5f)*a;
    }
    next.slope /= 60742.5f;
    if (!isfinite(next.slope)) return false;
    uint32_t coarse_step = phase_word(-(double)next.slope);
    float magnitude[23];
    for (unsigned block = 0; block < 23; ++block) {
        unsigned first = block ? 1530+1476*(block-1) : 0;
        unsigned count = block ? 36 : 90;
        complex sum = {0}, dsum = {0}, pilots[90];
        float energy = 0, denergy = 0;
        for (unsigned k = 0; k < count; ++k) {
            unsigned n = first+k;
            uint32_t phase = coarse_step*n;
            if (block) phase -= (quarter(gold, n)<<30) + (UINT32_C(1)<<29);
            else {
                unsigned q = header[n][0] > 0 ? (header[n][1] > 0 ? 0 : 3)
                                                      : (header[n][1] > 0 ? 1 : 2);
                phase -= (q<<30) + (UINT32_C(1)<<29);
            }
            complex v = known(frame[n], phase);
            pilots[k] = v; sum.r += v.r; sum.i += v.i;
            energy += v.r*v.r+v.i*v.i;
            if (derivative) {
                unsigned at = block ? 90+36*(block-1)+k : k;
                complex d = known(derivative[at], phase);
                dsum.r += d.r; dsum.i += d.i;
                denergy += 2*(v.r*d.r+v.i*d.i);
            }
        }
        complex mean = {sum.r/count, sum.i/count};
        next.phase[block] = atan2f(mean.i, mean.r);
        if (block) next.phase[block] = unwrap(next.phase[block], next.phase[block-1]);
        magnitude[block] = hypotf(mean.r, mean.i);
        if (!isfinite(magnitude[block])) return false;
        if (block)
            for (unsigned k = 0; k < count; ++k) {
                complex v = product(pilots[k], (complex){mean.r, -mean.i});
                next.pilot_errors += pilot_error(v.r, v.i);
            }
        if (derivative && energy > 0) {
            float norm = sum.r*sum.r+sum.i*sum.i;
            float coherence = norm/(count*energy);
            float gradient = (2*(sum.r*dsum.r+sum.i*dsum.i)-norm*denergy/energy)/(count*energy);
            if (isfinite(coherence) && isfinite(gradient)) {
                next.timing_coherence[block] = coherence;
                next.timing_gradient[block] = gradient;
                next.timing_observed |= UINT32_C(1)<<block;
            }
        }
    }
    for (unsigned n = 1; n < 23; ++n)
        for (unsigned j = n; j && magnitude[j] < magnitude[j-1]; --j) {
            float t = magnitude[j]; magnitude[j] = magnitude[j-1]; magnitude[j-1] = t;
        }
    next.amplitude = magnitude[11]*ROOT_HALF;
    if (!isfinite(next.amplitude) || next.amplitude <= 0 || next.amplitude > 1024) return false;
    next.high = (int16_t)ceil(31.0*(double)next.amplitude);
    next.negative_high = (int16_t)floor(-31.0*(double)next.amplitude);
    next.low = (int16_t)ceilf(-next.amplitude);
    for (unsigned s = 0; s < 22; ++s) {
        double delta = ((double)next.phase[s+1]-next.phase[s])/(anchor(s+1)-anchor(s));
        next.base[s] = phase_word(-next.phase[s]+anchor(s)*delta);
        next.step[s] = phase_word(-next.slope-delta);
    }
    next.prepared = true;
    *state = next;
    return true;
}

static int8_t classify(const rx_carrier *state, int v)
{
    return v >= state->high ? 24 : v <= state->negative_high ? -24 : v < state->low ? -8 : 8;
}

static bool project_checked(rx_carrier *state, const int16_t frame[RX_CARRIER_SYMBOLS][2],
                       const uint8_t gold[RX_CARRIER_GOLD_BYTES], unsigned symbols,
                       int8_t llr[RX_CARRIER_BITS], uint8_t word[RX_CARRIER_WORD_BYTES])
{
    if (!state || ((uintptr_t)state % _Alignof(rx_carrier)) || !state->prepared || state->failed || !frame || !gold || !llr || !word ||
        !symbols || symbols > 64 || state->cursor > RX_CARRIER_SYMBOLS ||
        state->emitted > RX_CARRIER_BITS || ((uintptr_t)frame & 1U)) return false;
    const void *p[] = {state, frame, gold, llr, word};
    const size_t size[] = {sizeof *state, RX_CARRIER_SYMBOLS*4U, RX_CARRIER_GOLD_BYTES,
                           RX_CARRIER_BITS, RX_CARRIER_WORD_BYTES};
    for (unsigned i = 0; i < 5; ++i)
        for (unsigned j = 0; j < i; ++j)
            if (!disjoint(p[i], size[i], p[j], size[j])) return false;
    unsigned end = state->cursor+symbols;
    if (end > RX_CARRIER_SYMBOLS) end = RX_CARRIER_SYMBOLS;
    int8_t header[90][2];
    if (state->cursor < 90 && !rx_carrier_header(state->metadata.modcod, header)) return false;
    unsigned segment = 0;
    for (unsigned n = state->cursor; n < end; ++n) {
        while (segment < 21 && 2*n > anchor_twice(segment+1)) ++segment;
        bool pilot = n >= 1530 && (n-1530)%1476 < 36;
        if (!pilot) {
            uint32_t phase = state->base[segment]+n*state->step[segment];
            if (n >= 90) phase -= quarter(gold, n)<<30;
            int16_t out[2]; rx_carrier_rotate(frame[n], phase, out);
            if (n < 90) {
                state->header_errors += (out[0] < 0) != (header[n][0] < 0);
                state->header_errors += (out[1] < 0) != (header[n][1] < 0);
            } else {
                if (state->emitted > RX_CARRIER_BITS-2) { state->failed = true; return false; }
                for (unsigned axis = 0; axis < 2; ++axis) {
                    unsigned bit = state->emitted++;
                    int8_t value = classify(state, out[axis]);
                    llr[bit] = value;
                    if (!(bit&7U)) word[bit/8] = 0;
                    word[bit/8] |= (uint8_t)((value < 0) << (7-(bit&7U)));
                }
            }
        }
        state->cursor = n+1;
    }
    return true;
}

static inline int floor_q5(int32_t value)
{
    /* Exact floor(value/65536), without negative shifts or negating INT_MIN. */
    return (int)(((uint32_t)value ^ UINT32_C(0x80000000)) >> 16)-32768;
}

static inline int level(int value, int high, int negative_high, int low)
{
    return value >= high ? 24 : value <= negative_high ? -24 : value < low ? -8 : 8;
}

static unsigned emitted_before(unsigned n)
{
    unsigned body = n-90, remainder = body%1476;
    unsigned pilots = 36*(body/1476)+(remainder > 1440 ? remainder-1440 : 0);
    return 2*(body-pilots);
}

bool rx_carrier_project(rx_carrier *state,
    const int16_t frame[RX_CARRIER_SYMBOLS][2],
    const uint8_t gold[RX_CARRIER_GOLD_BYTES], unsigned symbols,
    int8_t llr[RX_CARRIER_BITS], uint8_t word[RX_CARRIER_WORD_BYTES])
{
    /* Preserve reference behavior, including malformed-state partial failure,
     * header errors and every refusal, outside the normal data-only domain. */
    if (!state || ((uintptr_t)state % _Alignof(rx_carrier)) || !state->prepared || state->failed ||
        !frame || !gold || !llr || !word || !symbols || symbols > 64 ||
        state->cursor < 90 || state->cursor > RX_CARRIER_SYMBOLS ||
        state->emitted != emitted_before(state->cursor) || ((uintptr_t)frame & 1U))
        return project_checked(state, frame, gold, symbols, llr, word);
    const void *p[] = {state, frame, gold, llr, word};
    const size_t size[] = {sizeof *state, RX_CARRIER_SYMBOLS*4U, RX_CARRIER_GOLD_BYTES,
                          RX_CARRIER_BITS, RX_CARRIER_WORD_BYTES};
    for (unsigned i = 0; i < 5; ++i)
        for (unsigned j = 0; j < i; ++j)
            if (!disjoint(p[i], size[i], p[j], size[j]))
                return project_checked(state, frame, gold, symbols, llr, word);
    unsigned n = state->cursor, emitted = state->emitted, end = n+symbols;
    if (end > RX_CARRIER_SYMBOLS) end = RX_CARRIER_SYMBOLS;
    unsigned segment = 0;
    const int high = state->high, negative_high = state->negative_high, low = state->low;
    while (n < end) {
        unsigned body = n-90, offset = body%1476;
        if (offset >= 1440) {
            unsigned stop = n+1476-offset;
            n = stop < end ? stop : end;
            continue;
        }
        while (segment < 21 && 2*n > 3095+2952*segment) ++segment;
        unsigned stop = n+1440-offset;
        if (stop > end) stop = end;
        if (segment < 21) {
            unsigned boundary = (3095+2952*segment)/2+1;
            if (stop > boundary) stop = boundary;
        }
        uint32_t step = state->step[segment], phase = state->base[segment]+n*step;
        unsigned left = 4-(body&3U), packed = gold[body/4] >> (2*(body&3U));
        for (; n < stop; ++n) {
            uint32_t local = phase-((packed&3U)<<30);
            unsigned index = ((local+(UINT32_C(1)<<19)) >> 20)&4095U;
            int c = sine((index+1024)&4095U), s = sine(index);
            int32_t i = frame[n][0], q = frame[n][1];
            int a = level(floor_q5(i*c-q*s), high, negative_high, low);
            int b = level(floor_q5(i*s+q*c), high, negative_high, low);
            llr[emitted] = (int8_t)a;
            llr[emitted+1] = (int8_t)b;
            unsigned shift = 6-(emitted&7U), byte = emitted/8;
            unsigned signs = ((unsigned)(a < 0)<<1) | (b < 0);
            if (!(emitted&7U)) word[byte] = 0;
            word[byte] |= (uint8_t)(signs<<shift);
            emitted += 2;
            phase += step;
            packed >>= 2;
            if (!--left && n+1 < stop) {
                left = 4;
                packed = gold[(n+1-90)/4];
            }
        }
    }
    state->cursor = n;
    state->emitted = emitted;
    return true;
}
