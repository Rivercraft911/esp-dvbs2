#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RX_MC6_N 64800U
#define RX_MC6_K 43200U
#define RX_MC6_KBCH 43040U
#define RX_MC6_CHECKS 21600U
#define RX_MC6_WORD_BYTES 8100U
#define RX_MC6_PAYLOAD_BYTES 5370U
#define RX_MC6_TILES 2745U

/* Two normalized magnitudes; metadata = ten signs | last argmin << 10.
 * Bits 14..15 are zero. Messages are exactly -127..127, including at saturation. */
typedef struct {
    uint16_t magnitudes;
    uint16_t metadata;
} rx_mc6_check;

/* Caller owns distinct storage: 129600-byte beliefs and 86400-byte checks.
 * Information beliefs use natural order; parity uses 360*layer + column.
 * A cursor always describes the NEXT tile. iteration counts completed sweeps.
 * Stop between step calls to cancel/pause; resume with the same storage.
 * Neither storage nor decoder may be shared with another active invocation. */
typedef struct {
    int16_t *belief;
    rx_mc6_check *checks;
    uint32_t iteration;
    uint16_t layer;
    uint16_t column;
    bool initialized;
    bool failed;
} rx_mc6;

typedef struct {
    uint32_t unsatisfied;
    uint16_t payload_bytes;
    bool bch_ok;
    bool header_ok;
} rx_mc6_result;

/* init reads natural-order LLRs once and does not read/clear check storage.
 * Required alignments: belief 2 bytes, checks 4 bytes. Invalid ownership refuses
 * before changing any buffer. LLRs must remain separate during init only. */
bool rx_mc6_init(rx_mc6 *decoder, int16_t belief[RX_MC6_N],
                 rx_mc6_check checks[RX_MC6_CHECKS],
                 const int8_t llr[RX_MC6_N]);

/* At most tile_budget atomic tiles. Four active lanes in layer 0, eight elsewhere.
 * A returned count smaller than the budget indicates invalid state or a fault;
 * inspect failed. Zero budget is a no-op. No allocation or hidden yielding.
 * On ESP32-S3 the leaf masks interrupts and restores CPENABLE/SAR; all leaf
 * operands are internal stack storage. No QACC/ACCX or floating point is used. */
unsigned rx_mc6_step(rx_mc6 *decoder, unsigned tile_budget);

bool rx_mc6_pack(const rx_mc6 *decoder, uint8_t bits[RX_MC6_WORD_BYTES]);

/* Full LDPC syndrome, BCH160 remainder and all ten BBHEADER bytes. Supports
 * the host encoder's single-stream CCM GCS and TS profiles, no ISSY/NPD.
 * result must be distinct from bits. No correction or authentication is implied. */
bool rx_mc6_check_word(const uint8_t bits[RX_MC6_WORD_BYTES], rx_mc6_result *result);

/* Rechecks before publishing any bytes. Returns the descrambled BB data field,
 * including TS adaptation bytes when the header selects TS. Refusal leaves
 * payload and *bytes untouched. All output storage must be distinct. */
bool rx_mc6_extract(const uint8_t bits[RX_MC6_WORD_BYTES], uint8_t *payload,
                    size_t capacity, size_t *bytes);
