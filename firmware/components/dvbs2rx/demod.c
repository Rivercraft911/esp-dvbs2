/* DVB-S2 demodulator for the S3's radio dump: acquisition, timing, carrier, PL descrambling
 * and LLRs. ETSI EN 302 307-1 V1.4.1 clauses 5.5.2-5.5.4. Portable C99 with integers only, so
 * the S3 and a host build emit identical frames. The heavy loops are rx_unpack and rx_mf
 * (kernels.h) and demod_run below.
 *
 * Samples: 16 MS/s int8 I/Q, floor(s10 / 4), in a ring whose head is mirrored past its end so
 * every matched-filter window is contiguous. One 96-tap RRC output per symbol at the tracked
 * time, to 1/64 symbol, kept in a second ring. Each known block (PLHEADER or pilots) feeds the
 * carrier, timing, DC and leak loops as soon as it ends; the data run before it is then
 * queued with a carrier phase interpolated between the two blocks, and released to LLRs at
 * most DRAIN symbols per push so no call carries a whole run. */
#include "dvbs2rx.h"

#include <string.h>

#include "demod_tables.h"
#include "kernels.h"

#ifndef RX_DEMOD_NORMAL
#define RX_DEMOD_NORMAL 0         /* 1: also 64,800-bit frames (host tests on the corpus) */
#endif

#define RING 4096u                /* sample ring per axis, a power of two */
#define PAD 128u                  /* mirrored head: one rx_mf window plus 16 readable bytes */
#define CHUNK 2048u               /* words unpacked per pass of the consumers */
#define BATCH 128u                /* matched-filter outputs per rx_mf call */
#define HOLD 2560u                /* matched-filter outputs kept until their run is released */
#define DRAIN 896u                /* data symbols released per push call (8,100 per frame in 11) */
#define SPS 16u
#define HDR 90u                   /* PLHEADER symbols */
#define PIL 36u                   /* pilot block symbols */
#define SPAN 1440u                /* data symbols between pilot blocks (16 slots) */
#define MAX_SCR (RX_DEMOD_NORMAL ? 33192u : 8280u)   /* scrambled symbols per frame */
#define Q 24                      /* fractional bits of sample times */
#define ONE ((uint64_t)1 << Q)
#define EIGHTH (1u << 29)         /* pi/4; phases are 2^32 per turn */
#define SOF_MISS 4u               /* acquisition: sign misses allowed in 25 SOF terms */
#define ALL_MISS 20u              /* ... and in all 89 header terms */
#define LLR_UNIT 2965821          /* 32 sqrt(2) 2^16: an axis at amplitude a/sqrt(2) gives 32 */

#ifndef RX_CYCLES
#define RX_CYCLES() 0u            /* the firmware may supply a cycle counter */
#endif

enum { ACQUIRE, REFINE, TRACK };

typedef struct {
    uint32_t phase;               /* carrier phase at the block centre */
    int32_t theta;                /* phase residual against the prediction */
    int32_t amp;                  /* amplitude of the known symbols */
    int32_t tau;                  /* timing error, symbols Q16, > 0 = sampling late */
    int64_t sig, err;             /* known-symbol and error power */
    int32_t leak[2];              /* mean residual after the known symbols, Q4 */
} block_t;

typedef struct {
    uint32_t sc, n, d0;           /* first symbol's count, symbols, data index of the first */
    uint32_t phase, inc;          /* carrier phase at the first symbol, step per symbol */
    int32_t gain;                 /* LLR scale, Q16 */
    int32_t leak[8];              /* carrier leak after descrambling by R = 0..3 */
    uint32_t frame;               /* sequence number of its frame */
    int last;                     /* the frame's last run */
} run_t;

struct rx_demod {
    int8_t ri[RING + PAD] __attribute__((aligned(16)));
    int8_t rq[RING + PAD] __attribute__((aligned(16)));
    rx_mf_step_t steps[BATCH];
    int32_t mf[2 * BATCH];
    int16_t held[2 * (HOLD + 4)]; /* DC-free matched-filter outputs at symbol count % HOLD;
                                   * the first 4 repeat past the end so 4-symbol groups never
                                   * wrap */
    int16_t blk[2 * HDR];         /* one known block, linear */
    int32_t ref[2 * HDR];         /* that block derotated */
    uint8_t scr[MAX_SCR / 4];     /* PL scrambling R(i), four per byte, first in the low bits */
    rx_config_t cfg;
    uint32_t frame, data, pilots; /* symbols per frame, data symbols, pilot blocks */
    uint8_t hq[HDR];              /* PLHEADER symbol k = exp(j pi/4) j^hq[k] */
    uint8_t pls;                  /* expected MODCOD << 2 | TYPE */
    uint32_t pat[3];              /* expected differential signs, term 88 - j in bit j */
    int state;
    uint64_t written, valid;      /* sample ring holds [written - RING, written); stream start */

    /* acquisition: candidate c integrates samples [8c - 16, 8c) */
    uint64_t c, t0, first, pick;  /* next candidate; detection: first hit, chosen candidate */
    int32_t half[2], prev[2][2], acq_dc[2], dc_acc[2];
    uint32_t reg[2][3], fill[2], dc_n;
    unsigned pending, pick_miss;

    /* tracking */
    uint64_t t;                   /* next symbol time, Q24 dump samples */
    uint32_t step;                /* samples per symbol, Q24 */
    int32_t step_i;               /* timing loop integrator, Q24 */
    int32_t omega;                /* carrier rotation per symbol */
    uint32_t phase, anchor2, sc;  /* phase at the last block's centre, 2 x its symbol count;
                                   * count of the next symbol since lock */
    int32_t amp;                  /* amplitude of the last block */
    int32_t dc[2], leak[2];       /* Q4: receiver DC before derotation, TX carrier leak after */
    int32_t dc_sum[2];            /* DC-free outputs since the last block */
    uint32_t fk, since, d0;       /* frame index of the next symbol; symbols since the last
                                   * block; data index of the first data symbol after it */
    uint32_t hpos;                /* sc % HOLD */
    uint64_t now, hdr_t, frame_t; /* times: current symbol, latest header symbol 0, frame */
    int64_t sig, err;             /* frame MER sums */
    int in_frame, locked_once, fresh;
    uint32_t seq;
    uint64_t seq_t;               /* header time of the frame numbered seq */

    /* release */
    run_t runs[2];
    unsigned nrun;
    uint32_t done;                /* symbols of runs[0] already released */
    uint32_t wframe;              /* frame being written */
    int broken;                   /* that frame missed a buffer */
    rx_frame_t fin;               /* the queued last run's frame, without buffers */

    rx_frame_t *out;              /* this push call */
    size_t out_max, out_done;
    uint32_t budget;
    rx_demod_stats_t st;
};

/* ---------------------------------------------------------------- small integer helpers */

static unsigned popcount(uint32_t v)
{
    v = v - ((v >> 1) & 0x55555555u);
    v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
    return (((v + (v >> 4)) & 0x0f0f0f0fu) * 0x01010101u) >> 24;
}

static inline int32_t clamp127(int32_t v)
{
    return v > 127 ? 127 : v < -127 ? -127 : v;
}

/* (re, im) times exp(-j phase), Q14 table. |(re, im)| < 2^17 keeps the sums in int32. */
static inline void rotate(int32_t re, int32_t im, uint32_t phase, int32_t *ore, int32_t *oim)
{
    unsigned a = (phase + (1u << 21)) >> 22;
    int32_t c = rx_sin[(a + 256) & 1023], s = rx_sin[a & 1023];
    *ore = (re * c + im * s + 8192) >> 14;
    *oim = (im * c - re * s + 8192) >> 14;
}

/* (re, im) times j^-q. */
static inline void quad(int32_t *re, int32_t *im, unsigned q)
{
    int32_t r = *re, i = *im;
    switch (q & 3) {
    case 1: *re = i; *im = -r; break;
    case 2: *re = -r; *im = -i; break;
    case 3: *re = -i; *im = r; break;
    default: break;
    }
}

/* atan2(y, x), 2^32 per turn, by CORDIC; *mag gets the magnitude when asked. */
static uint32_t cordic(int64_t x, int64_t y, int64_t *mag)
{
    unsigned sh = 0;
    while (x > (1 << 29) || x < -(1 << 29) || y > (1 << 29) || y < -(1 << 29)) {
        x /= 2;
        y /= 2;
        ++sh;
    }
    int32_t X = (int32_t)x, Y = (int32_t)y;
    uint32_t a = 0;
    if (X < 0) {
        X = -X;
        Y = -Y;
        a = 1u << 31;
    }
    for (unsigned i = 0; i < 24; ++i) {
        int32_t dx = Y >> i, dy = X >> i;
        if (Y > 0) {
            X += dx;
            Y -= dy;
            a += rx_atan[i];
        } else {
            X -= dx;
            Y += dy;
            a -= rx_atan[i];
        }
    }
    if (mag) {
        *mag = ((int64_t)X * 39797 >> 16) << sh;   /* 1/1.6468 */
    }
    return a;
}

/* log2(v) in Q16, v > 0. */
static int32_t log2q16(uint64_t v)
{
    int n = 63;
    while (!(v >> n)) {
        --n;
    }
    uint64_t m = n >= 31 ? v >> (n - 31) : v << (31 - n);   /* [1, 2) in Q31 */
    int32_t r = n << 16;
    for (int b = 15; b >= 0; --b) {
        m = (m * m) >> 31;
        if (m >= (uint64_t)1 << 32) {
            m >>= 1;
            r |= 1 << b;
        }
    }
    return r;
}

/* ---------------------------------------------------------------- frame structure */

#define PLS_SCRAMBLE 0x719d83c953422dfaull

/* PLS code (5.5.2.4): b = MODCOD << 2 | TYPE selects rows of G; bit 63 is sent first. */
static uint64_t pls_code(unsigned b)
{
    static const uint32_t g[6] = {0x55555555u, 0x33333333u, 0x0f0f0f0fu,
                                  0x00ff00ffu, 0x0000ffffu, 0xffffffffu};
    uint32_t y = 0;
    for (unsigned k = 0; k < 6; ++k) {
        y ^= (b >> (6 - k) & 1) ? g[k] : 0;
    }
    uint64_t out = 0;
    for (unsigned i = 0; i < 32; ++i) {
        uint64_t bit = y >> (31 - i) & 1;
        out = out << 2 | bit << 1 | (bit ^ (b & 1));
    }
    return out ^ PLS_SCRAMBLE;
}

static inline uint32_t gold_x(uint32_t s)
{
    return s >> 1 | ((s ^ s >> 7) & 1) << 17;
}

static inline uint32_t gold_y(uint32_t s)
{
    return s >> 1 | ((s ^ s >> 5 ^ s >> 7 ^ s >> 10) & 1) << 17;
}

/* R(i) = 2 z(i + 131072) + z(i), z = x ^ y, code 0 (5.5.4). Registers hold x(i .. i + 17). */
static void scrambling(uint8_t *out, unsigned n)
{
    uint32_t x = 1, y = 0x3ffff, x2 = x, y2 = y;
    for (unsigned i = 0; i < 131072; ++i) {
        x2 = gold_x(x2);
        y2 = gold_y(y2);
    }
    for (unsigned i = 0; i < n; ++i) {
        unsigned r = 2 * ((x2 ^ y2) & 1) + ((x ^ y) & 1);
        out[i >> 2] |= (uint8_t)(r << (2 * (i & 3)));
        x = gold_x(x);
        y = gold_y(y);
        x2 = gold_x(x2);
        y2 = gold_y(y2);
    }
}

static inline unsigned scr_at(const uint8_t *scr, unsigned i)
{
    return scr[i >> 2] >> (2 * (i & 3)) & 3;
}

/* ---------------------------------------------------------------- estimates */

/* Samples per symbol, Q24. Both radios derive LO and sample clock from 40 MHz crystals, so
 * the clock offset is -CFO / f_c: step = 16 (1 - cfo / f_c), cfo = omega Rs / 2^32. */
static uint32_t base_step(const rx_demod_t *d)
{
    int64_t s = (int64_t)SPS << Q;
    if (d->cfg.carrier_khz) {
        s -= (int64_t)d->omega * 125 / (2 * (int64_t)d->cfg.carrier_khz);
    }
    return (uint32_t)(s + d->step_i);
}

static int32_t cfo_hz(const rx_demod_t *d)
{
    return (int32_t)((int64_t)d->omega * 62500 / (int64_t)(d->step ? d->step : SPS << Q));
}

static int32_t clock_ppb(const rx_demod_t *d)
{
    int64_t s = d->step ? d->step : (int64_t)SPS << Q;
    return (int32_t)((((int64_t)SPS << Q) - s) * 1000000000 / s);
}

/* ---------------------------------------------------------------- stream and ring */

static void acquire_from(rx_demod_t *d, uint64_t sample)
{
    d->state = ACQUIRE;
    d->c = sample / 8 + 3;
    d->fill[0] = d->fill[1] = 0;
    d->half[0] = d->half[1] = 0;
    d->pending = 0;
}

/* Sample s goes to ring byte s % RING; the first PAD bytes are mirrored past the end. */
static void put(rx_demod_t *d, const uint32_t *w, size_t n)
{
    while (n) {
        unsigned at = (unsigned)(d->written & (RING - 1));
        size_t k = RING - at < n ? RING - at : n;
        rx_unpack_span(w, k, d->ri + at, d->rq + at);
        if (at < PAD) {
            size_t end = at + k < PAD ? at + k : PAD;
            memcpy(d->ri + RING + at, d->ri + at, end - at);
            memcpy(d->rq + RING + at, d->rq + at, end - at);
        }
        d->written += k;
        w += k;
        n -= k;
    }
}

/* Matched-filter step for output time t: window samples n0 - 47 .. n0 + 48, n0 + p/4 = t,
 * rounded to a quarter sample. Only t mod RING samples matters, so 32 bits do. */
static inline void mf_step(rx_mf_step_t *s, uint64_t t)
{
    uint32_t tq = (uint32_t)((t + (ONE >> 3)) >> (Q - 2));
    uint32_t s0 = (tq >> 2) - RX_MF_CENTRE;
    s->base = s0 & (RING - 16);
    s->coef = rx_mf_sets + ((s0 & 15) * RX_MF_PHASES + (tq & 3)) * 16 * RX_MF_VECTORS;
}

/* Outputs from time t on whose windows are all written, at most `most`. */
static inline unsigned mf_ready(const rx_demod_t *d, uint64_t t, uint32_t step, unsigned most)
{
    uint64_t last = (t >> Q) + RX_MF_TAPS - RX_MF_CENTRE + 1;    /* past the window's end */
    if (last >= d->written) {
        return 0;
    }
    uint64_t n = (((d->written - last) << Q) - 1) / step + 1;
    return n < most ? (unsigned)n : most;
}

/* n DC-free outputs at t, t + step, ... as int16 pairs. */
static void mf_run(rx_demod_t *d, uint64_t t, uint32_t step, unsigned n, int16_t *y)
{
    int32_t dr = (d->dc[0] + 8) >> 4, di = (d->dc[1] + 8) >> 4;
    while (n) {
        unsigned b = n < BATCH ? n : BATCH;
        for (unsigned k = 0; k < b; ++k, t += step) {
            mf_step(&d->steps[k], t);
        }
        rx_mf(d->ri, d->rq, d->steps, b, RX_MF_VECTORS, d->mf);
        for (unsigned k = 0; k < b; ++k) {
            y[2 * k] = (int16_t)((d->mf[2 * k] >> RX_MF_SHIFT) - dr);
            y[2 * k + 1] = (int16_t)((d->mf[2 * k + 1] >> RX_MF_SHIFT) - di);
        }
        y += 2 * b;
        n -= b;
    }
}

/* ---------------------------------------------------------------- known blocks */

/* Soft-decode the PLS code from the derotated header z, with the phase of its SOF. The pilot
 * bit b7 decides how the 32 pairs combine; the pair sums are then a first-order Reed-Muller
 * (32, 6) word whose 32 correlations one Hadamard transform gives. */
static unsigned pls_decode(const rx_demod_t *d, const int32_t *z)
{
    int64_t cr = 0, ci = 0;
    for (unsigned k = 0; k < 26; ++k) {
        int32_t re = z[2 * k], im = z[2 * k + 1];
        quad(&re, &im, d->hq[k]);
        cr += re;
        ci += im;
    }
    while (cr > 32767 || cr < -32767 || ci > 32767 || ci < -32767) {
        cr /= 2;
        ci /= 2;
    }
    int32_t t[64];
    for (unsigned i = 0; i < 64; ++i) {
        int32_t re = z[2 * (26 + i)], im = z[2 * (26 + i) + 1];
        quad(&re, &im, (26 + i) & 1);
        t[i] = (int32_t)(((int64_t)re * cr + (int64_t)im * ci) >> 16);
        t[i] = (PLS_SCRAMBLE >> (63 - i) & 1) ? -t[i] : t[i];
    }
    unsigned best = 0;
    int32_t top = -1;
    for (unsigned b7 = 0; b7 < 2; ++b7) {
        int32_t r[32];
        for (unsigned m = 0; m < 32; ++m) {
            r[m] = b7 ? t[2 * m] - t[2 * m + 1] : t[2 * m] + t[2 * m + 1];
        }
        for (unsigned h = 1; h < 32; h <<= 1) {
            for (unsigned m = 0; m < 32; ++m) {
                if (!(m & h)) {
                    int32_t a = r[m], c = r[m + h];
                    r[m] = a + c;
                    r[m + h] = a - c;
                }
            }
        }
        for (unsigned a = 0; a < 32; ++a) {
            int32_t v = r[a] < 0 ? -r[a] : r[a];
            if (v > top) {
                top = v;
                /* a holds b1 in bit 0 .. b5 in bit 4; the sign gives b6 */
                best = (a & 1) << 6 | (a >> 1 & 1) << 5 | (a >> 2 & 1) << 4 | (a >> 3 & 1) << 3 |
                       (a >> 4 & 1) << 2 | (unsigned)(r[a] < 0) << 1 | b7;
            }
        }
    }
    return best;
}

/* Measure a known block of n DC-free outputs y (first symbol count sc0, symbol k =
 * exp(j pi/4) j^q[k]) against the carrier prediction. Returns 0 if it is not there. */
static int block(rx_demod_t *d, const int16_t *y, uint32_t sc0, unsigned n, const uint8_t *q,
                 int header, block_t *b)
{
    int32_t *z = d->ref;
    int32_t lr = (d->leak[0] + 8) >> 4, li = (d->leak[1] + 8) >> 4;
    uint32_t ph = d->phase + (uint32_t)((int64_t)d->omega * (int32_t)(2 * sc0 - d->anchor2) / 2);
    int64_t cr = 0, ci = 0, e = 0, sr0 = 0, si0 = 0, jr = 0, ji = 0;
    for (unsigned k = 0; k < n; ++k, ph += (uint32_t)d->omega) {
        int32_t re, im;
        rotate(y[2 * k], y[2 * k + 1], ph, &re, &im);
        z[2 * k] = re - lr;
        z[2 * k + 1] = im - li;
        sr0 += z[2 * k];
        si0 += z[2 * k + 1];
        jr += q[k] == 0 ? 1 : q[k] == 2 ? -1 : 0;
        ji += q[k] == 1 ? 1 : q[k] == 3 ? -1 : 0;
        re = z[2 * k];
        im = z[2 * k + 1];
        quad(&re, &im, q[k]);
        cr += re;
        ci += im;
        e += (int64_t)re * re + (int64_t)im * im;
    }
    /* A real block puts most of its power in the coherent sum. */
    int64_t c2 = cr * cr + ci * ci;
    if (!e || c2 < (header ? n * e / 2 : n * e / 4)) {
        return 0;
    }
    if (header && pls_decode(d, z) != d->pls) {
        return 0;
    }
    int64_t mag;
    b->theta = (int32_t)(cordic(cr, ci, &mag) - EIGHTH);
    b->amp = (int32_t)(mag / n);
    b->sig = c2 / n;
    b->err = e - b->sig > 0 ? e - b->sig : 1;
    b->phase = d->phase + (uint32_t)((int64_t)d->omega * (int32_t)(2 * sc0 + n - 1 - d->anchor2) / 2) +
               (uint32_t)b->theta;
    /* Leak error: mean of y - (C / n) j^q. */
    b->leak[0] = (int32_t)((sr0 - (cr * jr - ci * ji) / (int64_t)n) * 16 / (int64_t)n);
    b->leak[1] = (int32_t)((si0 - (cr * ji + ci * jr) / (int64_t)n) * 16 / (int64_t)n);
    /* Data-aided Mueller-Muller timing error: sum Re{c*_{k-1} y_k - c*_k y_{k-1}} after
     * derotation is about -1.78 tau A (n - 1) for an RRC cascade (slope 2 p'(1) of the raised
     * cosine at roll-off 0.35). With z = y j^-q and C = sum z this is sum Re{X conj C} / |C|. */
    int64_t sr = cr, si = ci;
    unsigned sh = 0;
    while (sr > 16383 || sr < -16383 || si > 16383 || si < -16383) {
        sr /= 2;
        si /= 2;
        ++sh;
    }
    int64_t ted = 0;
    for (unsigned k = 1; k < n; ++k) {
        int32_t ar = z[2 * k], ai = z[2 * k + 1], br = z[2 * k - 2], bi = z[2 * k - 1];
        quad(&ar, &ai, q[k - 1]);              /* j^dq z_k = j^-q[k-1] y_k */
        quad(&br, &bi, q[k]);                  /* j^-dq z_{k-1} = j^-q[k] y_{k-1} */
        ted += (int64_t)(ar - br) * sr + (int64_t)(ai - bi) * si;
    }
    int64_t den = (((int64_t)(n - 1) * (sr * sr + si * si) * 114) >> 6) << sh;
    int64_t tau = den ? -((int64_t)n * ted * 65536) / den : 0;
    b->tau = (int32_t)(tau > 16384 ? 16384 : tau < -16384 ? -16384 : tau);
    return 1;
}

/* ---------------------------------------------------------------- data runs */

/* The hot loops, for n DC-free outputs y of data symbols d .. d + n - 1 in groups of 4 (one
 * scrambling byte, one hard-bit byte); d and n are multiples of 4. W4 ports these to PIE.
 *
 * demod_turn: one complex multiply by exp(-j(phase + R pi/2)) both derotates and descrambles
 * (R from scr byte s / 4), then the carrier leak as descrambling turned it comes off. In
 * place: y's slots are free afterwards. */
static __attribute__((noinline)) void demod_turn(int16_t *y, unsigned n, uint32_t phase,
                                                 uint32_t inc, const int32_t leak[8],
                                                 const uint8_t *scr)
{
    for (unsigned g = 0; g < n / 4; ++g) {
        unsigned rs = scr[g];
        for (unsigned k = 0; k < 4; ++k, y += 2, rs >>= 2, phase += inc) {
            unsigned r = rs & 3, a = (phase + (r << 30)) >> 22;
            int32_t c = rx_sin[(a + 256) & 1023], sn = rx_sin[a], yr = y[0], yi = y[1];
            y[0] = (int16_t)(((yr * c + yi * sn + 8192) >> 14) - leak[2 * r]);
            y[1] = (int16_t)(((yi * c - yr * sn + 8192) >> 14) - leak[2 * r + 1]);
        }
    }
}

/* demod_llr: LLR scale (Q16), clipping to +-127, codeword order; hard bits are the LLR signs,
 * MSB first. llr and hard point at symbol d's bits. */
static __attribute__((noinline)) void demod_llr(const int16_t *u, unsigned n, int32_t gain,
                                                int8_t *llr, uint8_t *hard)
{
    for (unsigned g = 0; g < n / 4; ++g, u += 8, llr += 8) {
        unsigned bits = 0;
        for (unsigned k = 0; k < 8; ++k) {
            int32_t v = clamp127((u[k] * gain + 32768) >> 16);
            llr[k] = (int8_t)v;
            bits = bits << 1 | (uint32_t)v >> 31;
        }
        hard[g] = (uint8_t)bits;
    }
}

/* Queue the n data symbols before the block that just ended, with the phase line from the
 * previous block (phase, anchor2) at slope inc. */
static void queue_run(rx_demod_t *d, unsigned n, unsigned nblock, int32_t inc, int32_t amp, int last);

static void emit(rx_demod_t *d)
{
    if (d->broken || d->out_done >= d->out_max) {
        ++d->st.lost;
        return;
    }
    rx_frame_t *f = &d->out[d->out_done++];
    int8_t *llr = f->llr;
    uint8_t *hard = f->hard;
    *f = d->fin;
    f->llr = llr;
    f->hard = hard;
    ++d->st.frames;
}

/* Release up to limit queued data symbols, in groups of 4 (a frame ends the call's releases). */
static void drain(rx_demod_t *d, uint32_t limit)
{
    limit = (limit + 3) & ~3u;
    while (d->nrun && limit) {
        run_t *r = &d->runs[0];
        if (r->frame != d->wframe || (!d->done && !r->d0)) {
            d->wframe = r->frame;
            d->broken = 0;
        }
        rx_frame_t *f = d->out_done < d->out_max ? &d->out[d->out_done] : NULL;
        uint32_t at = (r->sc + d->done) % HOLD;
        uint32_t k = r->n - d->done;
        k = k < limit ? k : limit;
        k = (k < HOLD + 4 - at ? k : HOLD + 4 - at) & ~3u;
        if (!f || !f->llr || !f->hard) {
            d->broken = 1;                     /* no buffer: the frame is lost */
        } else {
            unsigned d0 = r->d0 + d->done;
            demod_turn(d->held + 2 * at, k, r->phase + r->inc * d->done, r->inc, r->leak,
                       d->scr + (d0 + PIL * (d0 / SPAN)) / 4);
            demod_llr(d->held + 2 * at, k, r->gain, f->llr + 2 * d0, f->hard + d0 / 4);
        }
        d->done += k;
        limit -= k;
        if (d->done == r->n) {
            int last = r->last;
            d->runs[0] = d->runs[1];
            --d->nrun;
            d->done = 0;
            if (last) {
                emit(d);
                limit = 0;                     /* the next frame's buffers come next call */
            }
        }
    }
    d->budget = limit < d->budget ? limit : d->budget;
}

static void queue_run(rx_demod_t *d, unsigned n, unsigned nblock, int32_t inc, int32_t amp, int last)
{
    if (d->nrun == 2) {
        drain(d, d->runs[0].n);                /* cannot happen at DRAIN >= 768 per call */
    }
    run_t *r = &d->runs[d->nrun++];
    r->sc = d->sc - nblock - n;
    r->n = n;
    r->d0 = d->d0;
    r->phase = d->phase + (uint32_t)((int64_t)inc * (int32_t)(2 * r->sc - d->anchor2) / 2) +
               (1u << 21);                     /* rounds the table index */
    r->inc = (uint32_t)inc;
    int32_t a = (d->amp + amp) / 2;
    r->gain = LLR_UNIT / (a > 91 ? a : 91);
    for (unsigned q = 0; q < 4; ++q) {
        int32_t lr = (d->leak[0] + 8) >> 4, li = (d->leak[1] + 8) >> 4;
        quad(&lr, &li, q);
        r->leak[2 * q] = lr;
        r->leak[2 * q + 1] = li;
    }
    r->frame = d->seq;
    r->last = last;
}

/* ---------------------------------------------------------------- frames */

static void finish_frame(rx_demod_t *d)
{
    rx_frame_t *f = &d->fin;
    memset(f, 0, sizeof *f);
    f->header_sample = (d->frame_t + (ONE >> 1)) >> Q;
    f->sequence = d->seq;
    f->cfo_hz = cfo_hz(d);
    f->clock_ppb = clock_ppb(d);
    /* 10 log10(sig / err) dB = 301.03 log2(sig / err) cdB */
    int64_t mer = ((int64_t)log2q16((uint64_t)d->sig) - log2q16((uint64_t)d->err)) * 301030 / 65536000;
    f->mer_cdb = (int16_t)(mer > 32767 ? 32767 : mer < -32767 ? -32767 : mer);
    f->modcod = (uint8_t)(d->pls >> 2);
    f->type = (uint8_t)(d->pls & 3);
    d->in_frame = 0;
}

static int lose(rx_demod_t *d)
{
    if (d->in_frame) {
        ++d->st.lost;
        while (d->nrun && d->runs[d->nrun - 1].frame == d->seq) {
            --d->nrun;                         /* its released part is never emitted */
            if (!d->nrun) {
                d->done = 0;
                d->broken = 1;
            }
        }
    }
    d->in_frame = 0;
    acquire_from(d, d->now >> Q);
    return 1;
}

/* A PLHEADER starts a frame: number it by frame periods since the first lock. */
static void start_frame(rx_demod_t *d, const block_t *b, uint64_t t)
{
    if (!d->locked_once) {
        d->locked_once = 1;
        d->seq = 0;
    } else {
        uint64_t period = (uint64_t)d->frame * d->step;
        uint32_t jump = (uint32_t)((t - d->seq_t + period / 2) / period);
        jump = jump ? jump : 1;
        d->st.lost += jump - 1;
        d->seq += jump;
    }
    d->seq_t = t;
    d->frame_t = t;
    d->in_frame = 1;
    d->d0 = 0;
    d->sig = b->sig;
    d->err = b->err;
}

/* Frequency: a quarter of the measured residual. Timing: half the error now (tau Q16 symbols
 * times 16 samples, Q24), and 1/256 of it per symbol of block spacing into the integrator,
 * which only has to absorb what -CFO / f_c misses. DC: a quarter of the mean residual since
 * the last block. Leak: an eighth of the block's residual. */
static void update_loops(rx_demod_t *d, const block_t *b, uint32_t centre2, int32_t inc)
{
    uint32_t span = (centre2 - d->anchor2) / 2;
    d->omega += (inc - d->omega) / 4;
    d->phase = b->phase;
    d->anchor2 = centre2;
    d->amp = b->amp;
    d->t -= (uint64_t)((int64_t)b->tau * (1 << (Q + 4 - 16 - 1)));
    if (span) {
        d->step_i -= (int32_t)((int64_t)b->tau * 16 / (int32_t)span);
        for (unsigned x = 0; x < 2; ++x) {
            d->dc[x] += (int32_t)((int64_t)d->dc_sum[x] * 16 / (int32_t)span) >> 2;
            d->dc_sum[x] = 0;
            d->leak[x] += b->leak[x] >> 3;
        }
    }
    d->step = base_step(d);
}

/* The header's carrier estimate can miss by a multiple of 1 / 1476 symbols (677 Hz at
 * 1 Msym/s), which every pilot phase then hides. So the first data run after a lock decides:
 * the fourth power of a QPSK symbol is unmodulated, and its phase slope over 32 blocks of 45
 * symbols (every third symbol, to bound the call) gives the residual rotation per symbol to
 * +-1/360 turn (+-2.8 kHz). */
static int64_t fourth_power_slope(rx_demod_t *d, unsigned nd, uint32_t sc0)
{
    int32_t lr = (d->leak[0] + 8) >> 4, li = (d->leak[1] + 8) >> 4;
    unsigned sh = 0;
    while ((d->amp >> sh) > 64) {
        ++sh;
    }
    uint32_t ph = d->phase + (uint32_t)((int64_t)d->omega * (int32_t)(2 * sc0 - d->anchor2) / 2);
    int64_t last = 0, sum = 0;
    for (unsigned b = 0; b < nd / 45; ++b) {
        int64_t br = 0, bi = 0;
        for (unsigned k = 45 * b; k < 45 * b + 45; k += 3, ph += 3 * (uint32_t)d->omega) {
            const int16_t *y = d->held + 2 * ((sc0 + k) % HOLD);
            int32_t re, im;
            rotate(y[0], y[1], ph, &re, &im);
            re = (re - lr) >> sh;
            im = (im - li) >> sh;
            int64_t r2 = (int64_t)re * re - (int64_t)im * im, i2 = 2 * (int64_t)re * im;
            br += r2 * r2 - i2 * i2;
            bi += 2 * r2 * i2;
        }
        uint32_t a = cordic(br, bi, NULL);
        last = b ? last + (int32_t)(a - (uint32_t)last) : (int32_t)a;
        sum += (int64_t)(2 * (int)b - 31) * last;             /* centres 45b + 22, symbols */
    }
    return sum / 982080;    /* slope / 4: sum (2b - 31)^2 = 10912, x 22.5 symbols x 4 */
}

/* A known block just ended. Returns 1 if the lock was lost. */
static int on_block(rx_demod_t *d, int header)
{
    unsigned n = header ? HDR : PIL, nd = d->since - n;
    uint32_t sc0 = d->sc - n;
    uint8_t pq[PIL];
    const uint8_t *q = d->hq;
    if (!header) {
        unsigned j = d->d0 / SPAN + 1;               /* pilot block number in the frame */
        for (unsigned k = 0; k < PIL; ++k) {
            pq[k] = (uint8_t)scr_at(d->scr, SPAN * j + PIL * (j - 1) + k);
        }
        q = pq;
    }
    for (unsigned k = 0; k < n; ++k) {
        const int16_t *y = d->held + 2 * ((sc0 + k) % HOLD);
        d->blk[2 * k] = y[0];
        d->blk[2 * k + 1] = y[1];
    }
    block_t b;
    if (!block(d, d->blk, sc0, n, q, header, &b)) {
        if (header && d->in_frame && d->d0 + nd == d->data) {
            /* The transmitter stopped after a whole frame: finish it on the frequency loop. */
            queue_run(d, nd, n, d->omega, d->amp, 1);
            finish_frame(d);
        }
        return lose(d);
    }
    uint32_t centre2 = 2 * sc0 + n - 1;
    int64_t theta = b.theta;
    if (d->fresh && !header) {
        /* Pick the turn count that agrees with the data's own rotation. */
        int64_t want = fourth_power_slope(d, nd, sc0 - nd) * (int32_t)(centre2 - d->anchor2) / 2;
        theta += ((want - theta + ((int64_t)1 << 31)) >> 32) * ((int64_t)1 << 32);
    }
    int32_t inc = d->omega + (int32_t)(theta * 2 / (int32_t)(centre2 - d->anchor2));
    if (d->fresh && !header) {
        d->omega = inc;
        d->fresh = 0;
    }
    if (nd) {
        queue_run(d, nd, n, inc, b.amp, header);
        d->d0 += nd;
    }
    if (header) {
        if (d->in_frame) {
            finish_frame(d);
        }
        start_frame(d, &b, d->hdr_t);
    } else {
        d->sig += b.sig;
        d->err += b.err;
    }
    update_loops(d, &b, centre2, inc);
    d->since = 0;
    return 0;
}

/* Symbols from the next one to the end of the next known block. Batches stop there, so each
 * block's corrections start at the symbol after it, however the dump arrives. */
static unsigned until_block(const rx_demod_t *d)
{
    if (d->fk < HDR) {
        return HDR - d->fk;
    }
    unsigned j = (d->fk - HDR) / (SPAN + PIL) + 1;
    return j <= d->pilots ? HDR + (SPAN + PIL) * j - d->fk : d->frame - d->fk + HDR;
}

static int track(rx_demod_t *d)
{
    for (;;) {
        unsigned left = until_block(d), n = mf_ready(d, d->t, d->step, left < BATCH ? left : BATCH);
        uint64_t t = d->t;
        if (!n) {
            return 0;
        }
        for (unsigned k = 0; k < n; ++k, t += d->step) {
            mf_step(&d->steps[k], t);
        }
        if (d->nrun && d->sc + n - (d->runs[0].sc + d->done) > HOLD) {
            drain(d, d->sc + n - (d->runs[0].sc + d->done) - HOLD);   /* keep the ring whole */
        }
        uint64_t t1 = d->t;
        uint32_t step = d->step;
        int32_t dr = (d->dc[0] + 8) >> 4, di = (d->dc[1] + 8) >> 4, sr = 0, si = 0;
        d->t = t;
        rx_mf(d->ri, d->rq, d->steps, n, RX_MF_VECTORS, d->mf);
        uint32_t hpos = d->hpos, fk = d->fk, wrap = 0;
        for (unsigned k = 0; k < n; ++k) {
            int16_t *y = d->held + 2 * hpos;
            y[0] = (int16_t)((d->mf[2 * k] >> RX_MF_SHIFT) - dr);
            y[1] = (int16_t)((d->mf[2 * k + 1] >> RX_MF_SHIFT) - di);
            sr += y[0];
            si += y[1];
            if (hpos < 4) {
                y[2 * HOLD] = y[0];
                y[2 * HOLD + 1] = y[1];
            }
            hpos = hpos + 1 < HOLD ? hpos + 1 : 0;
            if (++fk == d->frame) {
                fk = 0;
                wrap = k + 1;                          /* the next symbol starts a frame */
            }
        }
        d->hpos = hpos;
        d->fk = fk;
        if (wrap) {
            d->hdr_t = t1 + (uint64_t)wrap * step;
        }
        d->dc_sum[0] += sr;
        d->dc_sum[1] += si;
        d->sc += n;
        d->since += n;
        d->now = t1 + (uint64_t)(n - 1) * step;
        if (n == left && on_block(d, d->fk == HDR)) {      /* batches end at block ends */
            return 1;
        }
    }
}

/* ---------------------------------------------------------------- acquisition */

/* |sum_k conj(d_k) y_{k+1} conj(y_k)| over the first n header symbols: insensitive to the
 * carrier offset. */
static int64_t header_metric(const rx_demod_t *d, const int16_t *y, unsigned n)
{
    int64_t sr = 0, si = 0;
    for (unsigned k = 0; k + 1 < n; ++k) {
        int32_t ar = y[2 * k + 2], ai = y[2 * k + 3], br = y[2 * k], bi = y[2 * k + 1];
        int64_t vr = (int64_t)ar * br + (int64_t)ai * bi, vi = (int64_t)ai * br - (int64_t)ar * bi;
        switch ((d->hq[k + 1] - d->hq[k]) & 3) {
        case 1: sr += vi; si -= vr; break;
        case 3: sr -= vi; si += vr; break;
        default: sr += vr; si += vi; break;
        }
    }
    int64_t mag;
    cordic(sr, si, &mag);
    return mag;
}

/* Fine timing, carrier and phase from the header found near d->t0, then the header checks.
 * Returns 1 when tracking starts. */
static int refine(rx_demod_t *d)
{
    uint64_t t0 = d->t0;
    d->state = ACQUIRE;
    if ((t0 >> Q) < d->valid + 64 || (t0 >> Q) + RING < d->written + 128) {
        return 0;                                    /* the header is not all in the ring */
    }
    for (unsigned x = 0; x < 2; ++x) {
        d->dc[x] = (int32_t)((int64_t)d->acq_dc[x] * RX_MF_GAIN / 4096);
        d->leak[x] = 0;
    }
    /* Timing: the differential metric at -6 .. 6 samples in steps of 3 (the search picks a
     * candidate within 8), then the vertex of the parabola through the peak and its neighbours. */
    int16_t *y = d->blk;
    int64_t m[5];
    unsigned top = 0;
    for (unsigned o = 0; o < 5; ++o) {
        mf_run(d, t0 + (uint64_t)((int64_t)(3 * (int)o - 6) * (int64_t)ONE), SPS << Q, HDR / 2, y);
        m[o] = header_metric(d, y, HDR / 2) >> 8;
        top = m[o] > m[top] ? o : top;
    }
    int64_t off = (int64_t)(3 * (int)top - 6) * (int64_t)ONE;
    if (top > 0 && top < 4) {
        int64_t den = m[top - 1] - 2 * m[top] + m[top + 1];
        if (den < 0) {
            int64_t x = 3 * (m[top - 1] - m[top + 1]) * (int64_t)ONE / (2 * den), lim = 3 * (int64_t)ONE / 2;
            off += x > lim ? lim : x < -lim ? -lim : x;
        }
    }
    t0 += (uint64_t)off;
    mf_run(d, t0, SPS << Q, HDR, y);
    /* Carrier: w_k = y_k j^-q_k = A exp(j(pi/4 + phase + omega k)). Coarse rotation from the
     * lag-1 product, then a line through the phases of six 15-symbol sums. */
    int32_t *w = d->ref;
    int64_t sr = 0, si = 0;
    for (unsigned k = 0; k < HDR; ++k) {
        w[2 * k] = y[2 * k];
        w[2 * k + 1] = y[2 * k + 1];
        quad(&w[2 * k], &w[2 * k + 1], d->hq[k]);
        if (k) {
            sr += (int64_t)w[2 * k] * w[2 * k - 2] + (int64_t)w[2 * k + 1] * w[2 * k - 1];
            si += (int64_t)w[2 * k + 1] * w[2 * k - 2] - (int64_t)w[2 * k] * w[2 * k - 1];
        }
    }
    int32_t coarse = (int32_t)cordic(sr, si, NULL);
    int64_t ph[6], sum_ph = 0, sum_xy = 0;
    uint32_t p = (uint32_t)((int64_t)coarse * -(int64_t)(HDR - 1) / 2);   /* rotation at k = 0 */
    for (unsigned b = 0; b < 6; ++b) {
        int64_t br = 0, bi = 0;
        for (unsigned k = 15 * b; k < 15 * b + 15; ++k) {
            int32_t re, im;
            rotate(w[2 * k], w[2 * k + 1], p, &re, &im);
            br += re;
            bi += im;
            p += (uint32_t)coarse;
        }
        uint32_t a = cordic(br, bi, NULL);
        ph[b] = b ? ph[b - 1] + (int32_t)(a - (uint32_t)ph[b - 1]) : (int32_t)a;
        sum_ph += ph[b];
        sum_xy += (int64_t)(30 * (int)b - 75) * ph[b];          /* centres 15b + 7 - 44.5, doubled */
    }
    d->omega = coarse + (int32_t)(sum_xy / 7875);   /* slope 2 sum X phi / sum X^2, sum X^2 = 15750 */
    d->phase = (uint32_t)(sum_ph / 6) - EIGHTH;
    d->anchor2 = HDR - 1;
    d->step_i = 0;
    d->step = base_step(d);
    int64_t miss = (int64_t)cfo_hz(d) - d->cfg.cfo_hint_hz;
    block_t b;
    if (miss > (int64_t)d->cfg.cfo_span_hz || -miss > (int64_t)d->cfg.cfo_span_hz ||
        !block(d, y, 0, HDR, d->hq, 1, &b)) {
        ++d->st.rejects;
        return 0;
    }
    d->leak[0] = b.leak[0];                         /* the transmitter's carrier leak */
    d->leak[1] = b.leak[1];
    int64_t leak2 = ((int64_t)b.leak[0] * b.leak[0] + (int64_t)b.leak[1] * b.leak[1]) * HDR / 256;
    b.err = b.err > leak2 ? b.err - leak2 : 1;      /* the header was measured with the leak in */
    b.leak[0] = b.leak[1] = 0;
    d->fresh = 1;
    d->sc = HDR;
    d->hpos = HDR;
    d->fk = HDR;
    d->since = 0;
    d->dc_sum[0] = d->dc_sum[1] = 0;
    if (d->locked_once) {
        ++d->st.relocks;
    }
    start_frame(d, &b, t0);
    d->t = t0 + (uint64_t)HDR * d->step;
    d->now = d->t;
    update_loops(d, &b, HDR - 1, d->omega);
    d->state = TRACK;
    return 1;
}

/* Sum of the even-indexed 4 of 8 int8 samples (little-endian words, 8-aligned): one mask
 * per word on bytes offset to unsigned. Every other sample is plenty for detection, since the
 * signal lies far below the 4 MHz this folds. */
static inline int32_t half_sum(const int8_t *x)
{
    uint32_t a, b;
    x = __builtin_assume_aligned(x, 8);
    memcpy(&a, x, 4);
    memcpy(&b, x + 4, 4);
    uint32_t s = ((a ^ 0x80808080u) & 0x00ff00ffu) + ((b ^ 0x80808080u) & 0x00ff00ffu);
    return (int32_t)((s & 0xffffu) + (s >> 16)) - 512;
}

/* The search over the block of SEARCH candidates from c. Candidate c sums the even samples
 * of [8c - 16, 8c), centred at 8c - 9; acquire_from starts c after the stream start and the
 * consumers run every CHUNK words, so every window is in the ring. Fixed blocks keep the
 * result independent of how the dump arrives. Pass 1 forms the half sums; pass 2 runs each
 * timing phase's sign register in locals. A candidate is examined only if its 3 newest signs
 * match, then on the SOF's 25 and all 89. Returns 1 when a detection is chosen (state REFINE):
 * the fewest misses within 3 candidates of the first. */
#define SEARCH 64u
static int search(rx_demod_t *d)
{
    const uint32_t n = SEARCH;
    int32_t half[2 * SEARCH + 2];
    unsigned at = (unsigned)(8 * d->c - 8) & (RING - 1);
    int32_t ci = d->dc_acc[0], cq = d->dc_acc[1];
    half[0] = d->half[0];
    half[1] = d->half[1];
    for (uint32_t k = 0; k < n; ++k, at = (at + 8) & (RING - 1)) {
        int32_t si = half_sum(d->ri + at), sq = half_sum(d->rq + at);
        half[2 * k + 2] = si;
        half[2 * k + 3] = sq;
        ci += si;
        cq += sq;
        if (!(++d->dc_n & 255)) {   /* every 256 candidates, 1/8 of the mean, as a 16-sample sum */
            d->acq_dc[0] += (ci * 4 - d->acq_dc[0]) >> 3;
            d->acq_dc[1] += (cq * 4 - d->acq_dc[1]) >> 3;
            ci = cq = 0;
        }
    }
    d->dc_acc[0] = ci;
    d->dc_acc[1] = cq;
    d->half[0] = half[2 * n];
    d->half[1] = half[2 * n + 1];
    uint32_t hit_k[8];
    unsigned hit_m[8], hits = 0;
    for (unsigned ph = 0; ph < 2; ++ph) {
        unsigned x = ((unsigned)d->c ^ ph) & 1;        /* register of candidate c + ph */
        int32_t p0 = d->prev[x][0], p1 = d->prev[x][1];
        uint32_t r0 = d->reg[x][0], r1 = d->reg[x][1], r2 = d->reg[x][2], fill = d->fill[x];
        for (uint32_t k = ph; k < n; k += 2) {
            /* One sign bit per candidate: Im(a_c conj(a_{c-2})), the turn between symbols. */
            int32_t ai = half[2 * k + 2] + half[2 * k], aq = half[2 * k + 3] + half[2 * k + 1];
            uint32_t neg = (uint32_t)(aq * p0 - ai * p1) >> 31;
            p0 = ai;
            p1 = aq;
            r2 = r2 << 1 | r1 >> 31;
            r1 = r1 << 1 | r0 >> 31;
            r0 = r0 << 1 | neg;
            if (fill < HDR) {
                ++fill;
            } else if (!((r0 ^ d->pat[0]) & 7)) {
                unsigned miss = popcount((r2 ^ d->pat[2]) & 0x1ffffffu);
                if (miss <= SOF_MISS) {
                    miss += popcount(r0 ^ d->pat[0]) + popcount(r1 ^ d->pat[1]);
                    if (miss <= ALL_MISS && hits < 8) {
                        hit_k[hits] = k;
                        hit_m[hits++] = miss;
                    }
                }
            }
        }
        d->prev[x][0] = p0;
        d->prev[x][1] = p1;
        d->reg[x][0] = r0;
        d->reg[x][1] = r1;
        d->reg[x][2] = r2;
        d->fill[x] = fill;
    }
    for (unsigned h = 0; h < hits; ++h) {
        uint64_t k = d->c + hit_k[h];
        if (!d->pending) {
            d->pending = 1;
            d->first = d->pick = k;
            d->pick_miss = hit_m[h];
        }
        d->first = k < d->first ? k : d->first;
    }
    for (unsigned h = 0; h < hits; ++h) {
        uint64_t k = d->c + hit_k[h];
        if (k <= d->first + 3 && (hit_m[h] < d->pick_miss || (hit_m[h] == d->pick_miss && k < d->pick))) {
            d->pick_miss = hit_m[h];
            d->pick = k;
        }
    }
    d->c += n;
    if (!d->pending || d->c <= d->first + 3) {
        return 0;
    }
    /* Candidate c holds header symbol 89, centred at 8c - 9 samples; symbol 0 is at
     * 8c - 9 - 89 * 16 = 8c - 1433. */
    d->t0 = (uint64_t)(8 * d->pick - 1433) << Q;
    d->pending = 0;
    d->state = REFINE;
    return 1;
}

static int acquire(rx_demod_t *d)
{
    uint32_t start = RX_CYCLES();
    int locked = 0;
    for (;;) {
        if (d->state == REFINE) {
            if ((d->t0 >> Q) + (HDR - 1) * SPS + 6 + RX_MF_TAPS >= d->written) {
                break;                               /* the header's last window is not here yet */
            }
            if (refine(d)) {
                locked = 1;
                break;
            }
            continue;
        }
        if (8 * (d->c + SEARCH - 1) > d->written) {
            break;                                   /* the block's last window is not here */
        }
        search(d);
    }
    d->st.acquire_cycles += RX_CYCLES() - start;
    return locked;
}

/* ---------------------------------------------------------------- interface */

size_t rx_demod_size(void)
{
    return sizeof(rx_demod_t);
}

int rx_demod_init(rx_demod_t *d, const rx_config_t *config)
{
    if (!d || !config || config->modcod < 1 || config->modcod > 11 ||
        (config->normal && !RX_DEMOD_NORMAL)) {
        return -1;
    }
    memset(d, 0, sizeof *d);
    d->cfg = *config;
    if (!d->cfg.cfo_span_hz) {
        d->cfg.cfo_span_hz = 50000;
    }
    unsigned slots = config->normal ? 360 : 90;
    d->data = 90 * slots;
    d->pilots = (slots - 1) / 16;
    d->frame = HDR + d->data + PIL * d->pilots;
    d->pls = (uint8_t)(config->modcod << 2 | (config->normal ? 0 : 2) | 1);
    uint64_t code = pls_code(d->pls);
    for (unsigned k = 0; k < HDR; ++k) {
        unsigned y = k < 26 ? 0x18d2e82u >> (25 - k) & 1 : (unsigned)(code >> (63 - (k - 26)) & 1);
        d->hq[k] = (uint8_t)((2 * y + (k & 1)) & 3);
    }
    for (unsigned k = 0; k + 1 < HDR; ++k) {
        if (((d->hq[k + 1] - d->hq[k]) & 3) == 3) {
            d->pat[(88 - k) / 32] |= 1u << ((88 - k) % 32);
        }
    }
    scrambling(d->scr, d->frame - HDR);
    d->step = SPS << Q;
    d->wframe = ~0u;
    acquire_from(d, 0);
    return 0;
}

size_t rx_demod_push(rx_demod_t *d, const uint32_t *words, uint64_t first, size_t count,
                     rx_frame_t *out, size_t max)
{
    d->out = out;
    d->out_max = out ? max : 0;
    d->out_done = 0;
    d->budget = DRAIN;
    if (first != d->written) {
        if (d->in_frame) {
            ++d->st.lost;
        }
        d->in_frame = 0;
        d->written = d->valid = first;
        acquire_from(d, first);
    }
    d->st.words += count;
    while (count) {
        size_t k = count < CHUNK ? count : CHUNK;
        put(d, words, k);
        words += k;
        count -= k;
        while (d->state == TRACK ? track(d) : acquire(d)) {
        }
        drain(d, d->budget);
    }
    return d->out_done;
}

void rx_demod_stats(const rx_demod_t *d, rx_demod_stats_t *s)
{
    *s = d->st;
    s->locked = d->state == TRACK;
    s->cfo_hz = cfo_hz(d);
    s->clock_ppb = clock_ppb(d);
    /* 10-bit DC: s10 = 4 x + 1.5 for x = floor(s10 / 4). Locked, x's DC is dc / GAIN (dc in
     * Q4 of outputs that are sums >> 4); searching, it is acq_dc / 4096. In half LSB: */
    for (unsigned x = 0; x < 2; ++x) {
        int64_t v = s->locked ? (int64_t)d->dc[x] * 8 / RX_MF_GAIN : (int64_t)d->acq_dc[x] / 512;
        v += 3;
        *(x ? &s->dc_q : &s->dc_i) = (int16_t)(v >= 0 ? (v + 1) / 2 : (v - 1) / 2);
    }
}
