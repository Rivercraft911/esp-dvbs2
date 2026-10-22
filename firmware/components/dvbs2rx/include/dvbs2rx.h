#ifndef DVBS2RX_H
#define DVBS2RX_H

#include <stddef.h>
#include <stdint.h>

/* DVB-S2 receiver for the S3's own radio. Short FECFRAMEs (16,200 bits), QPSK,
 * pilots on, roll-off 0.35, PL scrambling code 0, GCS input, one MODCOD per run.
 * Portable C builds on the host; S3 PIE kernels must give identical integers.
 *
 * Input is the radio's dump stream: 16e6 complex samples/s, one 32-bit word each,
 * I in bits 19:10 and Q in bits 9:0, signed 10-bit (upper bits are not samples).
 * Core 1 pushes consecutive words into the demodulator, which emits frames.
 * Core 0 checks or decodes each frame and extracts its payload. */
#define RX_DUMP_RATE 16000000u
#define RX_SHORT_BITS 16200u
#define RX_HARD_BYTES 2032u          /* 2025 bytes of hard bits, padded for vector stores */
#define RX_PLHEADER_SYMBOLS 90u
#define RX_PILOT_SYMBOLS 36u         /* after every 16 slots of 90 symbols */
#define RX_QPSK_SHORT_SYMBOLS 8370u  /* 90 + 8100 + 5 * 36 */

static inline int rx_word_i(uint32_t w) { return (int32_t)(w << 12) >> 22; }
static inline int rx_word_q(uint32_t w) { return (int32_t)(w << 22) >> 22; }

/* One demodulated frame. Buffers belong to the frame ring; their memory is the
 * caller's choice (hard bits internal, LLRs may live in PSRAM: the clean path
 * never reads them). */
typedef struct {
    int8_t *llr;            /* RX_SHORT_BITS, codeword order, > 0 favours bit 0. Soft bits
                             * 32 x / a, clipped to +-127: a is the noise-free axis amplitude,
                             * so the true LLR is about llr * 10^(MER/10) / 16. */
    uint8_t *hard;          /* RX_HARD_BYTES, MSB first, bit = llr < 0 */
    uint64_t header_sample; /* dump index of the PLHEADER's first symbol centre */
    uint32_t sequence;      /* frames since lock; a jump means frames were lost */
    int32_t cfo_hz;         /* carrier offset estimate */
    int32_t clock_ppb;      /* TX symbol clock vs RX sample clock, parts per 1e9 */
    int16_t mer_cdb;        /* PLHEADER and pilot MER, 0.01 dB */
    uint8_t modcod;         /* decoded PLS code */
    uint8_t type;           /* PLS TYPE: bit 1 short frame, bit 0 pilots */
} rx_frame_t;

/* Demodulator: matched filter, timing, carrier, descrambling, LLRs. */
typedef struct {
    uint8_t modcod;         /* expected MODCOD, QPSK 4..10 */
    int32_t cfo_hint_hz;    /* centre of the acquisition search */
    uint32_t cfo_span_hz;   /* half-width of the search (0: 50 kHz) */
    uint32_t carrier_khz;   /* RF carrier: both radios run from 40 MHz crystals, so the symbol
                             * clock offset is -CFO / carrier (0: track it from timing alone) */
    uint8_t normal;         /* 64,800-bit frames (host tests on the normal-frame corpus): llr
                             * then holds 64,800 and hard 8,100 bytes */
} rx_config_t;

typedef struct {
    uint64_t words;         /* consumed */
    uint32_t frames, lost, relocks;
    uint32_t acquire_cycles;
    int16_t dc_i, dc_q;     /* current DC estimate, 10-bit LSB */
    uint8_t locked;
    int32_t cfo_hz, clock_ppb;  /* current estimates, as in rx_frame_t */
    uint32_t rejects;       /* acquisition detections that failed the header checks */
} rx_demod_stats_t;

typedef struct rx_demod rx_demod_t;
size_t rx_demod_size(void);
int rx_demod_init(rx_demod_t *d, const rx_config_t *config);
/* Consume count consecutive words starting at dump index first (a gap in first
 * forces a relock). LLRs and hard bits are written progressively, in order, into
 * out[0] while a frame is in progress, so keep out[0]'s llr/hard pointers stable
 * across calls until push returns a frame, then set fresh buffers. Returns the
 * number of complete frames in out (at most max; one per call when count is at
 * most 12,288 words). Real-time safe: bounded work per word, no allocation, no
 * blocking. Acquisition runs inline and is cheaper per word than tracking. */
size_t rx_demod_push(rx_demod_t *d, const uint32_t *words, uint64_t first, size_t count,
                     rx_frame_t *out, size_t max);
void rx_demod_stats(const rx_demod_t *d, rx_demod_stats_t *stats);

/* FEC: re-encode check first (clean frames end here), then layered min-sum.
 * rx_fec_size bytes in internal RAM hold everything the clean path touches; the
 * decoder's rx_fec_work_size bytes may live anywhere, PSRAM included. Both 16-byte
 * aligned. With work NULL, frames that fail the check return -1 undecoded. */
typedef struct {
    uint16_t iterations;    /* 0: the received hard bits were already a codeword */
    uint16_t failed_checks; /* parity checks failing on the received hard bits */
    uint8_t ldpc_ok, bch_ok, header_ok;
} rx_fec_stats_t;

typedef struct rx_fec rx_fec_t;
size_t rx_fec_size(unsigned modcod);
size_t rx_fec_work_size(unsigned modcod);
int rx_fec_init(rx_fec_t *f, unsigned modcod, void *work);
/* Returns the data field length in bytes and points payload at it (valid
 * until the frame's buffers are reused), or -1 if the frame fails. */
int rx_fec_frame(rx_fec_t *f, rx_frame_t *frame, const uint8_t **payload,
                 rx_fec_stats_t *stats);

#endif
