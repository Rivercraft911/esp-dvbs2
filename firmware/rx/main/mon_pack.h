#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The receiver's monitor datagrams (PVMN version 1), read by host/monitor.py on exp/monitor.
 * Portable, so tests/test_mon_pack.py checks the bytes. Little endian, each datagram:
 *   header, 12 bytes: "PVMN", u8 version 1, u8 type, u16 sequence (per type), u32 time (us)
 *   type 1 STATS:   UTF-8 JSON, at most MON_STATS_MAX bytes; unknown values are left out
 *   type 2 SAMPLES: u16 snapshot, u8 chunk, u8 chunks, u32 sample rate, then 512 complex int8
 *                   samples (I, Q): one chunk of 512 x chunks consecutive dump samples
 *   type 3 SYMBOLS: u16 count, u8 bits per symbol (2 or 3), u8 0, then count int8 I, Q pairs,
 *                   corrected symbols at a nominal constellation radius of 64 */

#define MON_HEADER 12u
#define MON_STATS 1u
#define MON_SAMPLES 2u
#define MON_SYMBOLS 3u
#define MON_STATS_MAX 1400u
#define MON_CHUNK 512u              /* complex samples per samples datagram */
#define MON_SAMPLES_BYTES (MON_HEADER + 8u + 2u * MON_CHUNK)    /* 1,044 */
#define MON_SYMBOLS_MAX 640u
#define MON_SYMBOLS_BYTES(n) (MON_HEADER + 4u + 2u * (n))

typedef struct {
    uint32_t run;
    bool track;                     /* "track", or "acquire" */
    uint16_t freq_mhz;
    uint32_t symbol_rate;           /* 1,000,000 or 5,000,000 */
    uint8_t modcod, gain;
    uint32_t frames, failed, lost, relocks, dummies, ldpc;
    uint32_t runs_dropped;          /* data runs the demodulator dropped: its release fell behind */
    uint32_t frames_dropped;        /* frames the receiver dropped: no free slot for the checker */
    bool have_mer;                  /* a frame has been received */
    int16_t mer_cdb, mer_med_cdb;   /* 0.01 dB: last frame, median of the last second */
    int32_t cfo_hz, clock_ppb;
    bool ts;
    uint32_t packets, crc8, cc, tei;
    bool eth;
    uint32_t eth_sent, eth_dropped, eth_q_peak, mon_dropped;
    uint16_t link_mbps;             /* the W5500's link: 10 or 100; 0 not known */
    int16_t core0_pmil, core1_pmil; /* busy, per mille; negative: not known */
    uint32_t capture_drops, heap_after_boot;
} mon_stats_t;

/* Each returns the datagram's length. out holds MON_HEADER + the body. */
size_t mon_header(uint8_t *out, unsigned type, uint16_t seq, uint32_t t_us);
size_t mon_stats(uint8_t *out, uint16_t seq, uint32_t t_us, const mon_stats_t *s);
size_t mon_samples(uint8_t *out, uint16_t seq, uint32_t t_us, uint16_t snapshot, unsigned chunk,
                   unsigned chunks, const int8_t *iq);
/* The same from I and Q planes of the whole snapshot (512 x chunks each), interleaved here. */
size_t mon_samples_planes(uint8_t *out, uint16_t seq, uint32_t t_us, uint16_t snapshot,
                          unsigned chunk, unsigned chunks, const int8_t *i, const int8_t *q);
size_t mon_symbols(uint8_t *out, uint16_t seq, uint32_t t_us, unsigned count, unsigned bits,
                   const int8_t *iq);
/* Dump words (I in bits 19:10, Q in 9:0, signed 10-bit) to int8 I, Q: floor(s10 / 4). */
void mon_unpack(int8_t *iq, const uint32_t *words, size_t n);
/* The median of n values (n <= 64; reorders them). */
int16_t mon_median(int16_t *v, unsigned n);
