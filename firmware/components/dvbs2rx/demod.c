/* DVB-S2 demodulator for the S3's radio dump: acquisition, timing, carrier, PL descrambling
 * and LLRs. ETSI EN 302 307-1 V1.4.1 clauses 5.5.2-5.5.4. Portable C99 with integers only, so
 * the S3 and a host build emit identical frames. The heavy loops are rx_unpack (kernels.h) and
 * the demod_pie.h kernels: demod_mf, demod_turn, demod_llr and demod_halves, with PIE
 * versions in demod_pie.S.
 *
 * Samples: 16 MS/s int8 I/Q, floor(s10 / 4), in a ring whose head is mirrored past its end so
 * every matched-filter window is contiguous. One 96-tap RRC output per symbol at the tracked
 * time, to 1/64 symbol, kept in a second ring. Each known block (PLHEADER or pilots) feeds the
 * carrier, timing, DC and leak loops as soon as it ends; the data run before it is then
 * queued with a carrier phase interpolated between the two blocks, and released to LLRs at
 * most DRAIN symbols per push so no call carries a whole run. */
#include "dvbs2rx.h"

#include <string.h>

#include "demod_pie.h"
#include "demod_tables.h"
#include "kernels.h"

#if DVBS2RX_PIE
#define demod_halves demod_halves_pie
#define demod_signs demod_signs_pie
#define demod_mf demod_mf_pie
#define demod_turn demod_turn_pie
#define demod_llr demod_llr_pie
#else
#define demod_halves demod_halves_c
#define demod_signs demod_signs_c
#define demod_mf demod_mf_c
#define demod_turn demod_turn_c
#define demod_llr demod_llr_c
#endif

#ifndef RX_DEMOD_NORMAL
#define RX_DEMOD_NORMAL 0         /* 1: also 64,800-bit frames (host tests on the corpus) */
#endif

#define RING DEMOD_RING         /* sample ring per axis, a power of two */
#define PAD (DEMOD_STRIDE - RING) /* mirrored head: at least a window plus 16 readable bytes */
#define CHUNK 2048u               /* words unpacked per pass of the consumers */
#define BATCH 128u                /* matched-filter outputs per demod_mf pass */
#define HOLD 2560u                /* data symbols kept until their run is released, 8k */
#define DRAIN 896u                /* data symbols released per push call (8,100 per frame in 11) */
#define SPS 16u
#define SEARCH 64u                /* acquisition candidates per block: two phases of 32 */
#define HDR 90u                   /* PLHEADER symbols */
#define PIL 36u                   /* pilot block symbols */
#define SPAN 1440u                /* data symbols between pilot blocks (16 slots) */
#define MAX_DATA (RX_DEMOD_NORMAL ? 32400u : 8100u)  /* data symbols per frame */
#define MAX_PIL (RX_DEMOD_NORMAL ? 792u : 180u)      /* pilot symbols per frame */
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
    int64_t sig, err;             /* known-symbol and error power, x 180 */
    int32_t leak[2];              /* mean residual after the known symbols, Q4 */
} block_t;

typedef struct {
    demod_run_t k;                /* the kernels' constants */
    uint32_t dw, n, d0;           /* data count of the first symbol, symbols, its data index */
    uint32_t phase, inc;          /* carrier phase at the first symbol (+ rounding), step */
    uint32_t frame;               /* sequence number of its frame */
    int last;                     /* the frame's last run */
} run_t;

struct rx_demod {
    int8_t ri[RING + PAD] __attribute__((aligned(16)));
    int8_t rq[RING + PAD] __attribute__((aligned(16)));
    demod_mf_t mf;
    int16_t yi[HOLD] __attribute__((aligned(16)));   /* data symbols' raw outputs, at data */
    int16_t yq[HOLD] __attribute__((aligned(16)));   /* count % HOLD; runs start 8-aligned */
    run_t runs[2];
    union {
        struct {
            int16_t blk[2 * HDR]; /* one known block: I, then Q at HDR */
            int32_t ref[2 * HDR]; /* that block derotated */
        };
        struct {                  /* the search's scratch, while no block is tracked */
            int16_t sh[2][SEARCH + 8] __attribute__((aligned(16)));  /* half sums */
            int16_t sx[4 * SEARCH] __attribute__((aligned(16)));    /* demod_signs' */
        };
    };
    uint8_t scr[MAX_DATA / 4 + 2] __attribute__((aligned(4)));  /* PL scrambling R of data */
    uint8_t scr_pil[MAX_PIL / 4]; /* ... and of pilots, four per byte, first in the low bits */
    rx_config_t cfg;
    uint32_t frame, data, pilots; /* symbols per frame, data symbols, pilot blocks */
    uint8_t hq[HDR];              /* PLHEADER symbol k = exp(j pi/4) j^hq[k] */
    uint8_t pls;                  /* expected MODCOD << 2 | TYPE */
    uint32_t pat[3];              /* expected differential signs, term 88 - j in bit j */
    int state;
    uint64_t written, valid;      /* sample ring holds [written - RING, written); stream start */

    /* acquisition: candidate c integrates samples [8c - 16, 8c) */
    uint64_t c, t0, first, pick;  /* next candidate; detection: first hit, chosen candidate */
    int16_t sa[16] __attribute__((aligned(16)));  /* demod_signs' a, carried between blocks */
    int16_t half[2];              /* the last block's last half sums */
    int32_t acq_dc[2], dc_acc[2];
    uint32_t reg[2][3], fill[2], dc_n;   /* per timing phase: signs, newest in reg[.][0] bit 0 */
    unsigned pending, pick_miss;

    /* tracking */
    uint64_t t;                   /* next symbol time, Q24 dump samples */
    uint32_t step;                /* samples per symbol, Q24 */
    int32_t step_i;               /* timing loop integrator, Q24 */
    int32_t omega;                /* carrier rotation per symbol */
    int32_t kstep;                /* clock seed per unit of omega, 2^-32 */
    uint32_t phase, anchor2, sc;  /* phase at the last block's centre, 2 x its symbol count;
                                   * count of the next symbol since lock */
    int32_t amp;                  /* amplitude of the last block */
    int32_t dc[2], leak[2];       /* Q4: receiver DC before derotation, TX carrier leak after */
    int32_t dc_sum[2];            /* raw outputs since the last block */
    uint32_t fk, since, d0;       /* frame index of the next symbol; symbols since the last
                                   * block; data index of the first data symbol after it */
    uint32_t dw, run_dw;          /* data symbols written; at the current run's start */
    uint64_t now, hdr_t, frame_t; /* times: current symbol, latest header symbol 0, frame */
    int64_t sig, err;             /* frame MER sums */
    int in_frame, locked_once, fresh;
    uint32_t seq;
    uint64_t seq_t;               /* header time of the frame numbered seq */

    /* release */
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

/* R(i) = 2 z(i + 131072) + z(i), z = x ^ y, code 0 (5.5.4), for the n symbols after the
 * header. Registers hold x(i .. i + 17). Data symbols' codes go to data in data order, pilot
 * blocks' (36 after every 1,440 data) to pil. */
static void scrambling(uint8_t *data, uint8_t *pil, unsigned n)
{
    uint32_t x = 1, y = 0x3ffff, x2 = x, y2 = y;
    for (unsigned i = 0; i < 131072; ++i) {
        x2 = gold_x(x2);
        y2 = gold_y(y2);
    }
    for (unsigned i = 0, nd = 0, np = 0; i < n; ++i) {
        unsigned r = 2 * ((x2 ^ y2) & 1) + ((x ^ y) & 1);
        if (i % (SPAN + PIL) < SPAN) {
            data[nd >> 2] |= (uint8_t)(r << (2 * (nd & 3)));
            ++nd;
        } else {
            pil[np >> 2] |= (uint8_t)(r << (2 * (np & 3)));
            ++np;
        }
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
 * the clock offset is CFO / f_c: step = 16 (1 - cfo / f_c), cfo = omega Rs / 2^32. */
static uint32_t base_step(const rx_demod_t *d)
{
    /* omega 62.5 / f_kHz as a high multiply: kstep = 2^32 62.5 / f_kHz, set at init */
    return (uint32_t)(((int32_t)SPS << Q) - (int32_t)(((int64_t)d->omega * d->kstep) >> 32) +
                      d->step_i);
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
    d->c = (sample / 8 + 3) | 1;                 /* odd: its window starts 16-aligned */
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

void demod_mf_c(demod_mf_t *m, unsigned n, int16_t *oi, int16_t *oq)
{
    uint64_t t = (uint64_t)m->t_hi << 32 | m->t_lo;
    for (unsigned k = 0; k < n; ++k, t += m->step) {
        /* window samples n0 - 47 .. n0 + 48 for output time n0 + p / 4 */
        uint32_t tq = (uint32_t)(t >> (Q - 2)), s0 = (tq >> 2) - RX_MF_CENTRE;
        const int8_t *xi = m->ri + (s0 & (RING - 16)), *xq = m->rq + (s0 & (RING - 16));
        const int8_t *h = m->sets + ((s0 & 15) * RX_MF_PHASES + (tq & 3)) * 16 * RX_MF_VECTORS;
        int32_t ai = 0, aq = 0;
        for (unsigned j = 0; j < 16 * RX_MF_VECTORS; ++j) {
            ai += xi[j] * h[j];
            aq += xq[j] * h[j];
        }
        oi[k] = (int16_t)(ai >> RX_MF_SHIFT);
        oq[k] = (int16_t)(aq >> RX_MF_SHIFT);
        m->sum_i += oi[k];
        m->sum_q += oq[k];
    }
    m->t_lo = (uint32_t)t;
    m->t_hi = (uint32_t)(t >> 32);
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

/* Point the matched filter at time t and step. */
static void mf_at(rx_demod_t *d, uint64_t t, uint32_t step)
{
    t += ONE >> 3;                               /* rounds to a quarter sample */
    d->mf.t_lo = (uint32_t)t;
    d->mf.t_hi = (uint32_t)(t >> 32);
    d->mf.step = step;
    d->mf.sum_i = d->mf.sum_q = 0;
}

/* Raw outputs less the DC estimate (refine's header). */
static void minus_dc(const rx_demod_t *d, int16_t *yi, int16_t *yq, unsigned n)
{
    int16_t dr = (int16_t)((d->dc[0] + 8) >> 4), di = (int16_t)((d->dc[1] + 8) >> 4);
    for (unsigned k = 0; k < n; ++k) {
        yi[k] = (int16_t)(yi[k] - dr);
        yq[k] = (int16_t)(yq[k] - di);
    }
}

/* ---------------------------------------------------------------- known blocks */

/* Soft-decode the PLS code from the derotated header z, with the phase of its SOF. The pilot
 * bit b7 decides how the 32 pairs combine; the pair sums are then a first-order Reed-Muller
 * (32, 6) word whose 32 correlations one Hadamard transform gives. */
static unsigned pls_decode(const rx_demod_t *d, const int32_t *z)
{
    int32_t cr = 0, ci = 0;
    for (unsigned k = 0; k < 26; ++k) {
        int32_t re = z[2 * k], im = z[2 * k + 1];
        quad(&re, &im, d->hq[k]);
        cr += re;
        ci += im;
    }
    while (cr > 16383 || cr < -16383 || ci > 16383 || ci < -16383) {
        cr /= 2;
        ci /= 2;
    }
    int32_t t[64];
    for (unsigned i = 0; i < 64; ++i) {
        int32_t re = z[2 * (26 + i)] >> 2, im = z[2 * (26 + i) + 1] >> 2;   /* < 2^14 */
        quad(&re, &im, (26 + i) & 1);
        t[i] = (re * cr + im * ci) >> 14;
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

/* Measure a known block of n raw outputs (first symbol count sc0, symbol k =
 * exp(j pi/4) j^q[k]) against the carrier prediction. Returns 0 if it is not there. */
static int block(rx_demod_t *d, const int16_t *yi, const int16_t *yq, uint32_t sc0, unsigned n,
                 const uint8_t *q, int header, block_t *b)
{
    int32_t *z = d->ref;
    int32_t lr = (d->leak[0] + 8) >> 4, li = (d->leak[1] + 8) >> 4;
    int32_t dr = (d->dc[0] + 8) >> 4, di = (d->dc[1] + 8) >> 4;
    uint32_t ph = d->phase + (uint32_t)((int64_t)d->omega * (int32_t)(2 * sc0 - d->anchor2) / 2);
    int32_t cr = 0, ci = 0, sr0 = 0, si0 = 0, jr = 0, ji = 0;   /* |z| < 2^16, n <= 90 */
    int64_t e = 0;
    for (unsigned k = 0; k < n; ++k, ph += (uint32_t)d->omega) {
        int32_t re, im;
        rotate(yi[k] - dr, yq[k] - di, ph, &re, &im);
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
    /* A real block puts most of its power in the coherent sum: |C|^2 >= n e / 2 (header) or
     * n e / 4 (pilots). */
    int64_t c2 = (int64_t)cr * cr + (int64_t)ci * ci;
    if (!e || (header ? 2 : 4) * c2 < (int64_t)n * e) {
        return 0;
    }
    if (header && pls_decode(d, z) != d->pls) {
        return 0;
    }
    int64_t mag;
    b->theta = (int32_t)(cordic(cr, ci, &mag) - EIGHTH);
    b->amp = (int32_t)mag / (int32_t)n;
    /* MER sums in 1/180 of the block power, 180 being a multiple of both block lengths:
     * signal |C|^2 / n and error e - |C|^2 / n. */
    b->sig = c2 * (int32_t)(180 / n);
    b->err = 180 * e - b->sig > 0 ? 180 * e - b->sig : 1;
    b->phase = d->phase + (uint32_t)((int64_t)d->omega * (int32_t)(2 * sc0 + n - 1 - d->anchor2) / 2) +
               (uint32_t)b->theta;
    /* Leak error: mean of y - (C / n) j^q. */
    b->leak[0] = (sr0 - (cr * jr - ci * ji) / (int32_t)n) * 16 / (int32_t)n;
    b->leak[1] = (si0 - (cr * ji + ci * jr) / (int32_t)n) * 16 / (int32_t)n;
    /* Data-aided Mueller-Muller timing error: sum Re{c*_{k-1} y_k - c*_k y_{k-1}} after
     * derotation is about -1.78 tau A (n - 1) for an RRC cascade (slope 2 p'(1) of the raised
     * cosine at roll-off 0.35). With z = y j^-q and C = sum z this is sum Re{X conj C} / |C|. */
    int32_t sr = cr, si = ci;
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

static inline int16_t sat16(int32_t v)
{
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

static inline int16_t mul14(int16_t a, int16_t b)
{
    return (int16_t)((int32_t)a * b >> 14);
}

void demod_run_init(demod_run_t *k, uint32_t inc, int16_t lr, int16_t li, int16_t dr, int16_t di,
                    int16_t gain)
{
    for (unsigned j = 0; j < 8; ++j) {
        unsigned b = (j * inc + (1u << 21)) >> 22;
        k->co[j] = rx_sin[(b + 256) & 1023];
        k->so[j] = rx_sin[b];
        k->lr[j] = lr;
        k->li[j] = li;
        k->dr[j] = dr;
        k->di[j] = di;
        k->sel[0][j] = (int16_t)(1u << 2 * j);
        k->sel[1][j] = (int16_t)(1u << (2 * j + 1));
        k->gain[j] = gain;
        k->hi[j] = 127;
        k->lo[j] = -127;
        k->weight[0][j] = (int16_t)(0x8000u >> j);
        k->weight[1][j] = (int16_t)(0x80u >> j);
        k->one[j] = 1;
    }
    k->table = rx_sin;
}

/* The release kernels (demod_pie.h), in the PIE's lane arithmetic. */
void demod_turn_c(int16_t *yi, int16_t *yq, unsigned groups, uint32_t phase, uint32_t step8,
                  const demod_run_t *k)
{
    for (unsigned g = 0; g < groups; ++g, yi += 8, yq += 8, phase += step8) {
        unsigned a = phase >> 22;
        int16_t cg = rx_sin[(a + 256) & 1023], sg = rx_sin[a];
        for (unsigned j = 0; j < 8; ++j) {
            int16_t c = sat16(mul14(cg, k->co[j]) - mul14(sg, k->so[j]));
            int16_t s = sat16(mul14(sg, k->co[j]) + mul14(cg, k->so[j]));
            int16_t xr = sat16(yi[j] - k->dr[j]), xi = sat16(yq[j] - k->di[j]);
            int16_t ur = sat16(mul14(xr, c) + mul14(xi, s));
            int16_t ui = sat16(mul14(xi, c) - mul14(xr, s));
            yi[j] = sat16(ur - k->lr[j]);
            yq[j] = sat16(ui - k->li[j]);
        }
    }
}

/* Descrambling multiplies by j^-R: (r, i) becomes (r, i), (i, -r), (-r, -i), (-i, r) for
 * R = 0..3. Lane masks from the code bits: `even` where R is even, `low` where R < 2. */
void demod_llr_c(const int16_t *yi, const int16_t *yq, unsigned n, const uint8_t *scr,
                 const demod_run_t *k, const demod_out_t *o)
{
    int8_t *llr = o->llr;
    uint8_t *hard = o->hard;
    for (unsigned g = 0; 8 * g < n; ++g, yi += 8, yq += 8, llr += 16, hard += 2) {
        unsigned w = scr[2 * g] | (unsigned)scr[2 * g + 1] << 8, bits = 0;
        for (unsigned j = 0; j < 8; ++j) {
            int16_t even = (w & (unsigned)k->sel[0][j]) ? 0 : -1;
            int16_t low = (w & (uint16_t)k->sel[1][j]) ? 0 : -1;
            int16_t e = (int16_t)((yi[j] ^ yq[j]) & even);
            int16_t a = (int16_t)(yq[j] ^ e), b = (int16_t)(yi[j] ^ e);
            int16_t na = (int16_t)~low, nb = (int16_t)(even ^ low);
            a = sat16((int16_t)(a ^ na) - na);
            b = sat16((int16_t)(b ^ nb) - nb);
            int16_t la = mul14(a, k->gain[j]), lb = mul14(b, k->gain[j]);
            la = la > 127 ? 127 : la < -127 ? -127 : la;
            lb = lb > 127 ? 127 : lb < -127 ? -127 : lb;
            if (8 * g + j < n) {
                llr[2 * j] = (int8_t)la;
                llr[2 * j + 1] = (int8_t)lb;
            }
            bits = bits << 2 | (unsigned)(la < 0) << 1 | (unsigned)(lb < 0);
        }
        hard[0] = (uint8_t)(bits >> 8);
        if (8 * g + 8 <= n) {
            hard[1] = (uint8_t)bits;
        }
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

/* Release up to limit queued data symbols, in groups of 8 (a frame ends the call's releases). */
static void drain(rx_demod_t *d, uint32_t limit)
{
    limit = (limit + 7) & ~7u;
    while (d->nrun && limit) {
        run_t *r = &d->runs[0];
        if (r->frame != d->wframe || (!d->done && !r->d0)) {
            d->wframe = r->frame;
            d->broken = 0;
        }
        rx_frame_t *f = d->out_done < d->out_max ? &d->out[d->out_done] : NULL;
        uint32_t at = (r->dw + d->done) % HOLD;
        uint32_t k = r->n - d->done;
        k = k < limit ? k : limit;
        k = k < HOLD - at ? k : HOLD - at;            /* runs start 8-aligned, HOLD is 8k */
        if (!f || !f->llr || !f->hard) {
            d->broken = 1;                     /* no buffer: the frame is lost */
        } else {
            unsigned d0 = r->d0 + d->done;
            demod_turn(d->yi + at, d->yq + at, (k + 7) / 8, r->phase + r->inc * d->done,
                       8 * r->inc, &r->k);
            demod_out_t o = {f->llr + 2 * d0, f->hard + d0 / 4};
            demod_llr(d->yi + at, d->yq + at, k, d->scr + d0 / 4, &r->k, &o);
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
    uint32_t sc = d->sc - nblock - n;
    r->dw = d->run_dw;
    r->n = n;
    r->d0 = d->d0;
    r->phase = d->phase + (uint32_t)((int64_t)inc * (int32_t)(2 * sc - d->anchor2) / 2) +
               (1u << 21);                     /* rounds the table index */
    r->inc = (uint32_t)inc;
    int32_t a = (d->amp + amp) / 2;
    demod_run_init(&r->k, (uint32_t)inc, (int16_t)((d->leak[0] + 8) >> 4),
                   (int16_t)((d->leak[1] + 8) >> 4), (int16_t)((d->dc[0] + 8) >> 4),
                   (int16_t)((d->dc[1] + 8) >> 4), (int16_t)(LLR_UNIT / 4 / (a > 91 ? a : 91)));
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
 * which only has to absorb what CFO / f_c misses. DC: a quarter of the mean residual since
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
        d->step_i -= b->tau * 16 / (int32_t)span;
        for (unsigned x = 0; x < 2; ++x) {           /* raw sums: < 1,476 * 2^15 * 16 */
            d->dc[x] += (d->dc_sum[x] * 16 / (int32_t)d->since - d->dc[x]) >> 2;
            d->dc_sum[x] = 0;
            d->leak[x] += b->leak[x] >> 3;
        }
    }
    d->step = base_step(d);
}

/* The header's carrier estimate can miss by a multiple of 1 / 1476 symbols (677 Hz at
 * 1 Msym/s), which every pilot phase then hides. So the first data run after a lock decides:
 * the fourth power of a QPSK symbol is unmodulated, and its phase slope over 16 blocks of 90
 * symbols (every fifth symbol, to bound the call) gives the residual rotation per symbol to
 * +-1/720 turn (+-1.4 kHz). */
static int64_t fourth_power_slope(rx_demod_t *d, unsigned nd, uint32_t sc0)
{
    int32_t lr = (d->leak[0] + 8) >> 4, li = (d->leak[1] + 8) >> 4;
    int32_t dr = (d->dc[0] + 8) >> 4, di = (d->dc[1] + 8) >> 4;
    unsigned sh = 0;
    while ((d->amp >> sh) > 16) {                /* 4th powers of < 2^6 sum below 2^30 */
        ++sh;
    }
    uint32_t ph = d->phase + (uint32_t)((int64_t)d->omega * (int32_t)(2 * sc0 - d->anchor2) / 2);
    int64_t last = 0, sum = 0;
    for (unsigned b = 0; b < nd / 90; ++b) {
        int32_t br = 0, bi = 0;
        for (unsigned k = 90 * b; k < 90 * b + 90; k += 5, ph += 5 * (uint32_t)d->omega) {
            unsigned at = (d->run_dw + k) % HOLD;
            int32_t re, im;
            rotate(d->yi[at] - dr, d->yq[at] - di, ph, &re, &im);
            re = (re - lr) >> sh;
            im = (im - li) >> sh;
            re = re > 63 ? 63 : re < -63 ? -63 : re;
            im = im > 63 ? 63 : im < -63 ? -63 : im;
            int32_t r2 = re * re - im * im, i2 = 2 * re * im;
            br += (r2 * r2 - i2 * i2) >> 4;
            bi += (2 * r2 * i2) >> 4;
        }
        uint32_t a = cordic(br, bi, NULL);
        last = b ? last + (int32_t)(a - (uint32_t)last) : (int32_t)a;
        sum += (int64_t)(2 * (int)b - 15) * last;             /* centres 90b + 42, symbols */
    }
    return sum / 244800;    /* slope / 4: sum (2b - 15)^2 = 1360, x 45 symbols x 4 */
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
            pq[k] = (uint8_t)scr_at(d->scr_pil, PIL * (j - 1) + k);
        }
        q = pq;
    }
    block_t b;
    if (!block(d, d->blk, d->blk + HDR, sc0, n, q, header, &b)) {
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
        d->budget = d->budget > 320 ? d->budget - 320 : 0;    /* it costs about 320 releases */
    }
    int32_t span = (int32_t)(centre2 - d->anchor2) / 2;    /* symbols between the centres */
    int32_t inc = d->omega + (theta == (int32_t)theta ? (int32_t)theta / span : (int32_t)(theta / span));
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
    d->dw = (d->dw + 7) & ~7u;                  /* the next run starts 8-aligned */
    d->run_dw = d->dw;
    return 0;
}

/* Where the next symbol falls. Returns the symbols up to the end of the next known block;
 * *nd of them are data before it begins, and the first block symbol is at *boff in it.
 * Batches stop at block ends, so each block's corrections start at the symbol after it,
 * however the dump arrives. */
static unsigned layout(const rx_demod_t *d, unsigned *nd, unsigned *boff)
{
    uint32_t fk = d->fk;
    *boff = 0;
    if (fk < HDR) {
        *nd = 0;
        *boff = fk;
        return HDR - fk;
    }
    unsigned p = fk - HDR, j = p / (SPAN + PIL), r = p - j * (SPAN + PIL);
    if (j >= d->pilots) {
        *nd = d->frame - fk;                         /* the last run, then the next header */
        return d->frame - fk + HDR;
    }
    *nd = r < SPAN ? SPAN - r : 0;
    *boff = r < SPAN ? 0 : r - SPAN;
    return SPAN + PIL - r;
}

static int track(rx_demod_t *d)
{
    for (;;) {
        unsigned nd, boff, left = layout(d, &nd, &boff);
        unsigned n = mf_ready(d, d->t, d->step, left < BATCH ? left : BATCH);
        if (!n) {
            return 0;
        }
        unsigned m = nd < n ? nd : n;
        if (m && d->nrun && d->dw + m - (d->runs[0].dw + d->done) > HOLD) {
            drain(d, d->dw + m - (d->runs[0].dw + d->done) - HOLD);   /* keep the ring whole */
        }
        uint64_t t1 = d->t;
        uint32_t step = d->step, at = d->dw % HOLD, c = m < HOLD - at ? m : HOLD - at;
        mf_at(d, t1, step);
        demod_mf(&d->mf, c, d->yi + at, d->yq + at);              /* data, wrapping the ring */
        demod_mf(&d->mf, m - c, d->yi, d->yq);
        demod_mf(&d->mf, n - m, d->blk + boff, d->blk + HDR + boff); /* known symbols */
        d->t = t1 + (uint64_t)n * step;
        d->dw += m;
        d->dc_sum[0] += d->mf.sum_i;
        d->dc_sum[1] += d->mf.sum_q;
        if (d->fk + n >= d->frame) {                   /* the next frame's header starts */
            d->hdr_t = t1 + (uint64_t)(d->frame - d->fk) * step;
            d->fk = d->fk + n - d->frame;
        } else {
            d->fk += n;
        }
        d->sc += n;
        d->since += n;
        d->now = t1 + (uint64_t)(n - 1) * step;
        if (n == left && on_block(d, d->fk == HDR)) {
            return 1;
        }
    }
}

/* ---------------------------------------------------------------- acquisition */

/* |sum_k conj(d_k) y_{k+1} conj(y_k)| over the first n header symbols: insensitive to the
 * carrier offset. */
static int64_t header_metric(const rx_demod_t *d, const int16_t *yi, const int16_t *yq, unsigned n)
{
    int32_t sr = 0, si = 0;
    for (unsigned k = 0; k + 1 < n; ++k) {                       /* y >> 4: sums < 2^29 */
        int32_t ar = yi[k + 1] >> 4, ai = yq[k + 1] >> 4, br = yi[k] >> 4, bi = yq[k] >> 4;
        int32_t vr = ar * br + ai * bi, vi = ai * br - ar * bi;
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
    int16_t *yi = d->blk, *yq = d->blk + HDR;
    int64_t m[5];
    unsigned top = 0;
    for (unsigned o = 0; o < 5; ++o) {
        mf_at(d, t0 + (uint64_t)((int64_t)(3 * (int)o - 6) * (int64_t)ONE), SPS << Q);
        demod_mf(&d->mf, HDR / 2, yi, yq);
        minus_dc(d, yi, yq, HDR / 2);
        m[o] = header_metric(d, yi, yq, HDR / 2) >> 8;
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
    mf_at(d, t0, SPS << Q);
    demod_mf(&d->mf, HDR, yi, yq);       /* raw: block() takes the DC off */
    /* Carrier: w_k = y_k j^-q_k = A exp(j(pi/4 + phase + omega k)). Coarse rotation from the
     * lag-1 product, then a line through the phases of six 15-symbol sums. */
    int32_t *w = d->ref;
    int32_t sr = 0, si = 0;
    for (unsigned k = 0; k < HDR; ++k) {
        w[2 * k] = yi[k] - ((d->dc[0] + 8) >> 4);
        w[2 * k + 1] = yq[k] - ((d->dc[1] + 8) >> 4);
        quad(&w[2 * k], &w[2 * k + 1], d->hq[k]);
        if (k) {                                                  /* w >> 5: sums < 2^28 */
            int32_t ar = w[2 * k] >> 5, ai = w[2 * k + 1] >> 5, br = w[2 * k - 2] >> 5, bi = w[2 * k - 1] >> 5;
            sr += ar * br + ai * bi;
            si += ai * br - ar * bi;
        }
    }
    int32_t coarse = (int32_t)cordic(sr, si, NULL);
    int64_t ph[6], sum_ph = 0, sum_xy = 0;
    uint32_t p = (uint32_t)((int64_t)coarse * -(int64_t)(HDR - 1) / 2);   /* rotation at k = 0 */
    for (unsigned b = 0; b < 6; ++b) {
        int32_t br = 0, bi = 0;
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
        !block(d, yi, yq, 0, HDR, d->hq, 1, &b)) {
        ++d->st.rejects;
        return 0;
    }
    d->leak[0] = b.leak[0];                         /* the transmitter's carrier leak */
    d->leak[1] = b.leak[1];
    int64_t leak2 = ((int64_t)b.leak[0] * b.leak[0] + (int64_t)b.leak[1] * b.leak[1]) * HDR * 180 / 256;
    b.err = b.err > leak2 ? b.err - leak2 : 1;      /* the header was measured with the leak in */
    b.leak[0] = b.leak[1] = 0;
    d->fresh = 1;
    d->sc = HDR;
    d->dw = d->run_dw = 0;
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

static const int8_t evens[32] __attribute__((aligned(16))) = {
    1, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0};

/* Every other sample is plenty for detection: the signal lies far below the 4 MHz it folds. */
void demod_halves_c(const int8_t *xi, const int8_t *xq, unsigned n, int16_t *hi, int16_t *hq,
                    const int8_t *w)
{
    (void)w;                                     /* the PIE's form of the same sums */
    for (unsigned k = 0; k < n; ++k, xi += 8, xq += 8) {
        hi[k] = (int16_t)(xi[0] + xi[2] + xi[4] + xi[6]);
        hq[k] = (int16_t)(xq[0] + xq[2] + xq[4] + xq[6]);
    }
}

uint64_t demod_signs_c(const int16_t *hi, const int16_t *hq, int16_t *a, int32_t *sums,
                       int16_t *scratch)
{
    (void)scratch;                               /* the PIE's staging */
    int32_t i2 = a[6], q2 = a[14], i1 = a[7], q1 = a[15];        /* a_{k-2}, a_{k-1} */
    uint64_t bits = 0;
    for (unsigned k = 0; k < SEARCH; ++k) {
        int32_t ai = hi[k] + hi[k + 1], aq = hq[k] + hq[k + 1];
        bits = bits << 1 | (uint32_t)(aq * i2 - ai * q2) >> 31;
        i2 = i1;
        q2 = q1;
        i1 = ai;
        q1 = aq;
        sums[0] += hi[k + 1];
        sums[1] += hq[k + 1];
        if (k >= SEARCH - 8) {
            a[k - (SEARCH - 8)] = (int16_t)ai;
            a[k - (SEARCH - 16)] = (int16_t)aq;
        }
    }
    return bits;
}

/* Lock at t0, or unless config.quick_lock on the next header: one frame later at the clock
 * the CFO hint implies (short frames: +-2 samples for +-30 kHz), so a search and a refine
 * never share a call. */
static void defer_lock(rx_demod_t *d)
{
    if (!d->cfg.quick_lock) {
        d->omega = (int32_t)(uint32_t)((int64_t)d->cfg.cfo_hint_hz * 4295);   /* 2^32 / 1e6 */
        d->t0 += (uint64_t)d->frame * base_step(d);
        d->omega = 0;
    }
    d->state = REFINE;
}

/* (hi:lo) >> s for s < 32 */
static inline uint32_t funnel(uint32_t hi, uint32_t lo, unsigned s)
{
    return s ? hi << (32 - s) | lo >> s : lo;
}

/* Bits 63, 61, .., 1 of x as bits 31 .. 0. */
static uint32_t odd_bits(uint64_t x)
{
    x = x >> 1 & 0x5555555555555555ull;
    x = (x | x >> 1) & 0x3333333333333333ull;
    x = (x | x >> 2) & 0x0f0f0f0f0f0f0f0full;
    x = (x | x >> 4) & 0x00ff00ff00ff00ffull;
    x = (x | x >> 8) & 0x0000ffff0000ffffull;
    return (uint32_t)(x | x >> 16);
}

/* One timing phase's 32 new signs w (its first candidate in bit 31) against the header:
 * candidate j's register is (r2:r1:r0:w) >> (31 - j). Every candidate whose 3 newest signs
 * match is found at once from the shifted words; only those count misses, on the SOF's 25 and
 * then all 89. Hits append to (k, m) in candidate order. */
static unsigned match(rx_demod_t *d, unsigned ph, uint32_t w, uint32_t *hit_k, unsigned *hit_m,
                      unsigned hits)
{
    uint32_t r0 = d->reg[ph][0], r1 = d->reg[ph][1], r2 = d->reg[ph][2], fill = d->fill[ph];
    uint32_t p = d->pat[0];
    d->reg[ph][2] = r1;
    d->reg[ph][1] = r0;
    d->reg[ph][0] = w;
    d->fill[ph] = fill + 32 < HDR ? fill + 32 : HDR;
    if (fill + 31 < HDR) {
        return hits;                             /* no register is complete yet */
    }
    uint32_t m = ~(w ^ (0u - (p & 1))) & ~((w >> 1 | r0 << 31) ^ (0u - (p >> 1 & 1))) &
                 ~((w >> 2 | r0 << 30) ^ (0u - (p >> 2 & 1)));
    m &= ~0u >> (fill >= HDR ? 0 : HDR - fill);  /* candidates from HDR - fill */
    while (m) {
        unsigned s = 31 - (unsigned)__builtin_clz(m);
        m ^= 1u << s;
        unsigned miss = popcount((funnel(r2, r1, s) ^ d->pat[2]) & 0x1ffffffu);
        if (__builtin_expect(miss <= SOF_MISS, 0)) {
            miss += popcount(funnel(r0, w, s) ^ p) + popcount(funnel(r1, r0, s) ^ d->pat[1]);
            if (miss <= ALL_MISS && hits < 8) {
                hit_k[hits] = ph + 2 * (31 - s);
                hit_m[hits++] = miss;
            }
        }
    }
    return hits;
}

/* The search over the block of SEARCH candidates from c. Candidate c sums the even samples
 * of [8c - 16, 8c), centred at 8c - 9; acquire keeps c's window in the ring. Fixed blocks
 * keep the result independent of how the dump arrives. demod_halves forms the half sums,
 * demod_signs the sign of the turn between symbols, Im(a_c conj(a_{c-2})), per candidate;
 * then each timing phase's 32 signs are matched. Returns 1 when a detection is chosen (state
 * REFINE): the fewest misses within 3 candidates of the first. */
static int search(rx_demod_t *d)
{
    const uint32_t n = SEARCH;
    int16_t *hi = d->sh[0], *hq = d->sh[1];
    unsigned at = (unsigned)(8 * d->c - 8) & (RING - 1), part = (RING - at) / 8;  /* c odd */
    part = part < n ? part : n;
    hi[0] = d->half[0];
    hq[0] = d->half[1];
    demod_halves(d->ri + at, d->rq + at, part, hi + 1, hq + 1, evens);
    demod_halves(d->ri, d->rq, n - part, hi + 1 + part, hq + 1 + part, evens);
    uint64_t signs = demod_signs(hi, hq, d->sa, d->dc_acc, d->sx);
    if (!((d->dc_n += n) & 255)) {      /* every 256 candidates, 1/8 of the mean, as a 16-sample sum */
        d->acq_dc[0] += (d->dc_acc[0] * 4 - d->acq_dc[0]) >> 3;
        d->acq_dc[1] += (d->dc_acc[1] * 4 - d->acq_dc[1]) >> 3;
        d->dc_acc[0] = d->dc_acc[1] = 0;
    }
    d->half[0] = hi[n];
    d->half[1] = hq[n];
    uint32_t hit_k[8];
    unsigned hit_m[8];
    unsigned hits = match(d, 0, odd_bits(signs), hit_k, hit_m, 0);
    hits = match(d, 1, odd_bits(signs << 1), hit_k, hit_m, hits);
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
    defer_lock(d);
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
        if (8 * d->c + RING < d->written + 16) {
            acquire_from(d, d->written);             /* a failed deferred refine left c a frame
                                                      * back: search the new samples, not the
                                                      * ring's overwritten past */
            continue;
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
    scrambling(d->scr, d->scr_pil, d->frame - HDR);
    if (config->carrier_khz) {
        d->kstep = (int32_t)(((int64_t)125 << 31) / config->carrier_khz);
    }
    d->mf.ri = d->ri;
    d->mf.rq = d->rq;
    d->mf.sets = rx_mf_sets;
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
