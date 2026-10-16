#ifndef DVBS2_H
#define DVBS2_H

#include <stddef.h>
#include <stdint.h>

/* Normal frames only. Bits are packed MSB first; phase k means exp(j*k*pi/4).
 * SIS/CCM, roll-off 0.35, no ISSY/NPD, PL scrambling code 0.
 * Use mode pointers from dvbs2_find_mode or dvbs2_modes. Payload/input and
 * phase output buffers must not overlap the workspace or TS state. */
#define DVBS2_FEC_BYTES 8100
#define DVBS2_MAX_SYMBOLS 33282

typedef struct {
    const char *rate;
    uint16_t kbch, nbch;
    uint8_t modcod, bits_per_symbol, q, t;
} dvbs2_mode;

extern const dvbs2_mode dvbs2_modes[];
extern const size_t dvbs2_mode_count;
const dvbs2_mode *dvbs2_find_mode(unsigned modcod);
size_t dvbs2_frame_symbols(const dvbs2_mode *mode, int pilots);

typedef struct {
    uint8_t bits[DVBS2_FEC_BYTES];
    uint32_t parity[90][12]; /* q rows, 360 useful LSB-first bits per row */
} dvbs2_workspace;

typedef struct {
    uint16_t packet_pos; /* raw TS byte offset, 0..187 */
    uint8_t previous_crc, crc;
} dvbs2_ts_state;

/* Zero initialization starts a TS stream (the first replacement CRC is zero).
 * Each call consumes exactly (kbch-80)/8 original TS bytes. Packet sync 0x47 is
 * replaced by the previous packet CRC. State continues across BBFRAMEs.
 * BB functions return 0 on success, -1 for invalid input; on error they do not
 * modify the workspace or TS state. GCS accepts arbitrary dfl_bits, including 0. */
int dvbs2_bb_gcs(dvbs2_workspace *ws, const dvbs2_mode *mode,
                const uint8_t *payload, size_t dfl_bits);
int dvbs2_bb_ts(dvbs2_workspace *ws, const dvbs2_mode *mode,
               dvbs2_ts_state *state, const uint8_t *input, size_t input_bytes);

/* Public stages permit bit-for-bit checks and per-stage cycle measurements.
 * Call in this order after bb_gcs/bb_ts. bits[] contains BBFRAME, then scrambled
 * BBFRAME, BCH codeword, and LDPC codeword respectively. */
void dvbs2_bb_scramble(dvbs2_workspace *ws, const dvbs2_mode *mode);
void dvbs2_bch(dvbs2_workspace *ws, const dvbs2_mode *mode);
void dvbs2_ldpc(dvbs2_workspace *ws, const dvbs2_mode *mode);
/* Fuses the 8PSK column interleaver with mapping; QPSK has no interleaver. */
void dvbs2_map(const dvbs2_workspace *ws, const dvbs2_mode *mode,
               uint8_t *data_phases);
/* Map to output+90 first. Expands in place, adds header and pilots, scrambles
 * the body. Allocate dvbs2_frame_symbols(mode,pilots) bytes. */
void dvbs2_plframe(const dvbs2_mode *mode, int pilots, uint8_t *output);

/* Fast encoder (fast.c): the same frames with wide tables, whole-row LDPC and
 * table mapping. Allocate dvbs2_fast_size() bytes, 16-byte aligned, in
 * internal RAM (about 90 KB), and prepare it for one mode and pilot setting.
 * The pie mask selects ESP32-S3 vector kernels (pie.S) per stage; without PIE
 * support the mask is ignored and portable C runs. dvbs2_fast_pie() tells
 * which kernels are in use; dvbs2_fast_ldxq_bias() reports the measured
 * EE.LDXQ.32 address offset (0 or 4 bytes, -1 if unused). Stages keep
 * ws->bits in the canonical format: bb_* writes the scrambled BBFRAME, then
 * bch and ldpc as above. map writes the mapped and scrambled data symbols to
 * their PLFRAME positions and may scribble on the others; pl then writes the
 * header and pilots. output: dvbs2_frame_symbols(mode, pilots) bytes, any
 * alignment. */
typedef struct dvbs2_fast dvbs2_fast;
enum { DVBS2_PIE_BB = 1, DVBS2_PIE_BCH = 2, DVBS2_PIE_LDPC = 4, DVBS2_PIE_MAP = 8 };
size_t dvbs2_fast_size(void);
int dvbs2_fast_prepare(dvbs2_fast *f, const dvbs2_mode *mode, int pilots, unsigned pie);
unsigned dvbs2_fast_pie(const dvbs2_fast *f);
int dvbs2_fast_ldxq_bias(const dvbs2_fast *f);
int dvbs2_fast_bb_gcs(const dvbs2_fast *f, dvbs2_workspace *ws,
                      const uint8_t *payload, size_t dfl_bits);
int dvbs2_fast_bb_ts(const dvbs2_fast *f, dvbs2_workspace *ws, dvbs2_ts_state *state,
                     const uint8_t *input, size_t input_bytes);
void dvbs2_fast_bch(dvbs2_fast *f, dvbs2_workspace *ws);
void dvbs2_fast_ldpc(dvbs2_fast *f, dvbs2_workspace *ws);
void dvbs2_fast_map(dvbs2_fast *f, const dvbs2_workspace *ws, uint8_t *output);
void dvbs2_fast_pl(const dvbs2_fast *f, uint8_t *output);

#endif
