#ifndef DVBS2RX_H
#define DVBS2RX_H

#include <stddef.h>
#include <stdint.h>

/* DVB-S2 receiver for the S3's own radio. Short FECFRAMEs (16,200 bits), QPSK or 8PSK at
 * 1 or 5 Msym/s, pilots on, roll-off 0.35, PL scrambling code 0, GCS or TS input, one MODCOD
 * per run, dummy PLFRAMEs between frames.
 * Portable C builds on the host; S3 PIE kernels must give identical integers.
 *
 * Input is the radio's dump stream: 16e6 complex samples/s, one 32-bit word each,
 * I in bits 19:10 and Q in bits 9:0, signed 10-bit (upper bits are not samples).
 * Core 1 pushes consecutive words into the demodulator: matched filter, synchronization and
 * a ring of raw symbols. Core 0 releases the ring's data runs into frames of hard bits (carrier
 * removal, descrambling, demapping), then checks or decodes each frame. */
#define RX_DUMP_RATE 16000000u
#define RX_SHORT_BITS 16200u
#define RX_HARD_BYTES 2032u          /* 2025 bytes of hard bits, padded for vector stores */
#define RX_PLHEADER_SYMBOLS 90u
#define RX_PILOT_SYMBOLS 36u         /* after every 16 slots of 90 symbols */
#define RX_QPSK_SHORT_SYMBOLS 8370u  /* 90 + 8100 + 5 * 36 */

static inline int rx_word_i(uint32_t w) { return (int32_t)(w << 12) >> 22; }
static inline int rx_word_q(uint32_t w) { return (int32_t)(w << 22) >> 22; }

/* One demodulated frame. Buffers are the caller's; hard bits are what the clean path reads.
 * Soft bits are 32 d / d0 for each bit: d is the received point's signed distance to that
 * bit's decision boundary, d0 the noise-free point's nearest one (a / sqrt(2) for QPSK,
 * a sin(pi/8) for 8PSK, a the constellation radius). */
typedef struct {
    int8_t *llr;            /* optional (NULL on the board): RX_SHORT_BITS soft bits clipped to
                             * +-127, codeword order, > 0 favours bit 0; for QPSK the true LLR
                             * is about llr * 10^(MER/10) / 16 */
    uint8_t *hard;          /* RX_HARD_BYTES, codeword order, MSB first, bit = soft bit < 0 */
    uint8_t *weak;          /* optional: RX_HARD_BYTES, as hard, set where |soft bit| < 16 */
    uint64_t header_sample; /* dump index of the PLHEADER's first symbol centre */
    uint32_t sequence;      /* frames since lock; a jump means frames were lost */
    int32_t cfo_hz;         /* carrier offset estimate */
    int32_t clock_ppb;      /* TX symbol clock vs RX sample clock, parts per 1e9 */
    int16_t mer_cdb;        /* PLHEADER and pilot MER, 0.01 dB */
    uint8_t modcod;         /* decoded PLS code */
    uint8_t type;           /* PLS TYPE: bit 1 short frame, bit 0 pilots */
} rx_frame_t;

/* Demodulator: matched filter, timing, carrier, descrambling, demapping. */
typedef struct {
    uint8_t modcod;         /* expected MODCOD: QPSK 4..11, 8PSK 12..17 */
    uint32_t symbol_rate;   /* 1,000,000 (or 0) or 5,000,000 symbols/s */
    int32_t cfo_hint_hz;    /* centre of the acquisition search */
    uint32_t cfo_span_hz;   /* half-width of the search (0: 50 kHz) */
    uint32_t carrier_khz;   /* RF carrier: both radios run from 40 MHz crystals, so the symbol
                             * clock offset is CFO / carrier (0: track it from timing alone) */
    uint8_t normal;         /* 64,800-bit frames (host tests on the normal-frame corpus): llr
                             * then holds 64,800, hard and weak 8,100 bytes */
    uint8_t quick_lock;     /* lock on the PLHEADER found rather than the next one: one more
                             * frame per acquisition, but the search and the lock then share
                             * a call (for recorded captures) */
} rx_config_t;

typedef struct {
    uint64_t words;         /* consumed */
    uint32_t frames, lost, relocks;  /* frames released; lost on either core */
    uint32_t acquire_cycles;
    int16_t dc_i, dc_q;     /* current DC estimate, 10-bit LSB */
    uint8_t locked;
    int32_t cfo_hz, clock_ppb;  /* current estimates, as in rx_frame_t */
    uint32_t rejects;       /* acquisition detections that failed the header checks */
    uint32_t dropped;       /* frames lost because the release fell behind */
    uint32_t dummies;       /* dummy PLFRAMEs: ridden through when locked, hopped over to lock */
} rx_demod_stats_t;

typedef struct rx_demod rx_demod_t;
size_t rx_demod_size(void);
/* ring: RX_DEMOD_RING_BYTES, 16-byte aligned, internal RAM: raw data symbols, 4,096 of them
 * (0.82 ms at 5 Msym/s), between push and release. */
#define RX_DEMOD_RING_BYTES 16384u
int rx_demod_init(rx_demod_t *d, const rx_config_t *config, void *ring);
/* Core 1. Consume count consecutive words starting at dump index first (a gap in first
 * forces a relock). Matched-filter outputs of data symbols go to a ring, and each data run
 * is queued for rx_demod_release once the known block after it has fixed its carrier.
 * Returns the number of runs queued. Real-time safe: bounded work per word, no allocation,
 * no blocking; if the release falls behind, frames are dropped. Acquisition runs inline. */
size_t rx_demod_push(rx_demod_t *d, const uint32_t *words, uint64_t first, size_t count);
/* Core 0, concurrently with push (one thread each). Releases queued runs into frame: hard
 * bits (and weak bits, soft bits where the pointers are set) written in place as runs
 * arrive, so keep frame's buffers until it returns 1 with a complete frame, then set fresh
 * ones. Returns 0 when the queue is empty. */
int rx_demod_release(rx_demod_t *d, rx_frame_t *frame);
void rx_demod_stats(const rx_demod_t *d, rx_demod_stats_t *stats);

/* FEC: re-encode check first (clean frames end here), then layered min-sum.
 * rx_fec_size bytes in internal RAM hold everything the clean path touches; the
 * decoder's rx_fec_work_size bytes may live anywhere, PSRAM included. Both 16-byte
 * aligned. With work NULL, frames that fail the check return -1 undecoded. The decoder
 * reads the frame's soft bits, or without them two levels from its hard and weak bits.
 * 8PSK MODCODs use the code of the QPSK MODCOD with the same rate. */
typedef struct {
    uint16_t iterations;    /* 0: the received hard bits were already a codeword */
    uint16_t failed_checks; /* parity checks failing on the received hard bits */
    uint16_t unsatisfied;   /* checks failing in the last iteration (stops: 8 iterations, or no
                             * progress over two from the third, or more than a sixth failing) */
    uint8_t ldpc_ok, bch_ok, header_ok;
} rx_fec_stats_t;

typedef struct rx_fec rx_fec_t;
size_t rx_fec_size(unsigned modcod);
size_t rx_fec_check_size(unsigned modcod);  /* enough when work is NULL */
size_t rx_fec_work_size(unsigned modcod);
int rx_fec_init(rx_fec_t *f, unsigned modcod, void *work);
/* Returns the data field length in bytes and points payload at it (valid
 * until the frame's buffers are reused), or -1 if the frame fails. The BBHEADER, descrambled,
 * is the 10 bytes before it. Accepted streams: generic continuous (TS/GS = 01, UPL, SYNCD 0)
 * and transport stream (TS/GS = 11, UPL 188 x 8, SYNC 0x47, SYNCD a byte offset inside the data
 * field or 65,535), single stream, ISSY and null-packet deletion off for TS. */
int rx_fec_frame(rx_fec_t *f, rx_frame_t *frame, const uint8_t **payload,
                 rx_fec_stats_t *stats);

/* TS deframer: single transport stream BBFRAMEs (EN 302 307-1 5.1.4 to 5.1.6) back into
 * 188-byte packets, in order, each checked by the CRC-8 that the next packet's sync position
 * carries. Packets that straddle frames are carried over. A packet whose CRC-8 fails goes on
 * with its transport_error_indicator set. A sequence gap, a late frame, a frame that is not TS
 * or a SYNCD that disagrees with the carried packet restarts at the frame's SYNCD: a complete
 * packet waiting for its CRC-8 then goes on unchecked, a partial one is dropped. Continuity
 * counters (ISO/IEC 13818-1 2.4.3.3) are checked for the first RX_TS_PIDS PIDs. */
#define RX_TS_PACKET 188u
#define RX_TS_PIDS 16u

typedef struct {
    uint32_t frames;        /* TS data fields taken */
    uint32_t packets;       /* handed to emit */
    uint32_t crc_errors;    /* CRC-8 mismatches, handed on with transport_error_indicator set */
    uint32_t unchecked;     /* handed on at a gap or the end, before their CRC-8 arrived */
    uint32_t null_packets;  /* PID 0x1fff, whether dropped or not */
    uint32_t cc_errors;     /* continuity counter jumps over all PIDs */
    uint32_t resyncs;       /* restarts at a SYNCD after the first start */
    uint32_t syncd_errors;  /* ... of them because SYNCD disagreed with the carried packet */
    uint32_t not_ts;        /* frames that were not single-TS BBFRAMEs */
    uint32_t late;          /* frames not newer than the last one taken: ignored */
    uint32_t dropped_bytes; /* data field bytes before a first packet start, or partial packets */
    uint32_t untracked;     /* packets of PIDs past the first RX_TS_PIDS: no continuity check */
} rx_ts_stats_t;

typedef struct {
    uint16_t pid;
    uint32_t packets, cc_errors;
} rx_ts_pid_t;

/* One packet, 188 bytes from its 0x47, valid only during the call. */
typedef void (*rx_ts_emit_fn)(void *ctx, const uint8_t *packet);

typedef struct rx_ts rx_ts_t;
size_t rx_ts_size(void);
/* drop_null: null packets are counted but not handed on. */
void rx_ts_init(rx_ts_t *t, int drop_null, rx_ts_emit_fn emit, void *ctx);
/* One frame that rx_fec_frame accepted: bbframe is its descrambled BBHEADER, followed by the
 * data field of `bytes` bytes, and sequence is rx_frame_t.sequence. The packets it completes go
 * to emit before it returns: from the data field itself where they lie inside it (their sync
 * bytes restored in place, so the field changes) or from the deframer's own copy. Returns the
 * number handed on. */
unsigned rx_ts_frame(rx_ts_t *t, uint8_t *bbframe, unsigned bytes, uint32_t sequence);
/* The end of the stream: a complete packet waiting for its CRC-8 goes on unchecked. */
void rx_ts_flush(rx_ts_t *t);
void rx_ts_stats(const rx_ts_t *t, rx_ts_stats_t *stats);
/* The i-th PID seen, in order of first appearance; 0 if there are fewer. */
int rx_ts_pid(const rx_ts_t *t, unsigned i, rx_ts_pid_t *pid);

#endif
