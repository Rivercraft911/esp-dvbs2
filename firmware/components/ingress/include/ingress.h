#ifndef INGRESS_H
#define INGRESS_H

/* Payload input: fixed-size PV messages land in a ring of DMA slots, are checked here, and are
 * handed to the encoder as a byte stream. Portable C99; the ESP32-S3 SPI slave that fills the
 * ring is in ingress_spi.c, and tests/ingress_sim.c fills it on the host.
 *
 * PV message (1332 bytes, little-endian fields), PV-SPI v1:
 *   0 magic "PV" (0x50 0x56), 2 version 1, 3 type, 4 seq (+1 per message, wraps),
 *   6 payload length, 8 reserved, 12 payload (1316 bytes, zero padded),
 *   1328 CRC-32 of bytes 0..1327 (IEEE, zlib.crc32).
 * Types: 0 NOP (length 0), 1 TS (188*n bytes, n = 1..7, every packet starts 0x47),
 *        2 GCS (1..1316 bytes of a generic continuous stream).
 *
 * Ring ownership. Every slot has one GDMA descriptor whose owner bit says who may touch it:
 *   owner = 1  the DMA may write the slot (armed, empty or being filled);
 *   owner = 0  the DMA has finished with it (the hardware clears the bit on end-of-frame or when
 *              the buffer is full); from then on only the consumer reads it.
 * The consumer takes slots strictly in ring order. A slot that failed a check is released at
 * once. An accepted message stays with the consumer until it has been read completely
 * (ingress_read) or until the next ingress_next call; then the consumer sets owner = 1, links the
 * slot behind the current tail of the DMA chain and asks the hardware to continue. Only the
 * consumer writes descriptors; the hardware only clears owner and fills length and end-of-frame.
 * READY is high while at least INGRESS_READY_MARGIN slots are armed, so a sender that sampled
 * READY slightly late still finds a free slot for one message. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PV_MSG_BYTES 1332
#define PV_HEADER_BYTES 12
#define PV_PAYLOAD_MAX 1316
#define PV_TS_PACKET 188

enum { PV_NOP = 0, PV_TS = 1, PV_GCS = 2 };

typedef enum {
    PV_OK = 0,
    PV_SHORT,   /* transaction ended before 1332 bytes */
    PV_LONG,    /* more than 1332 bytes in one transaction */
    PV_HEADER,  /* magic, version, type or length */
    PV_CRC,
    PV_SYNC,    /* a TS packet without 0x47 */
} pv_status;

/* zlib-compatible: pv_crc32(0, p, n) == zlib.crc32(p[:n]); pass a previous result to continue. */
uint32_t pv_crc32(uint32_t crc, const void *data, size_t n);
/* Exclusive caller-owned 1024-word CRC table; NULL restores the table-free path.
 * Set before starting input, release only after stopping all readers. */
void pv_crc32_tables(uint32_t *table);
/* Fill the header, zero the padding after the payload already at msg+12, append the CRC. */
void pv_seal(uint8_t *msg, unsigned type, uint16_t seq, size_t payload_bytes);
/* Check one received transaction of len bytes. */
pv_status pv_check(const uint8_t *msg, size_t len);

/* One word longer than a message, so a 1332-byte transaction never fills a slot exactly and its
 * end-of-frame always lands in the same descriptor. */
#define INGRESS_SLOT_BYTES 1336
#define INGRESS_SLOTS 4
#define INGRESS_READY_MARGIN 2

/* ESP32-S3 GDMA descriptor, TRM v1.8 section 3.4.1: size [11:0], length [23:12],
 * suc_eof [30], owner [31]; buffer; next (NULL ends the chain). */
typedef struct ingress_desc {
    volatile uint32_t dw0;
    uint8_t *buf;
    struct ingress_desc *volatile next;
} ingress_desc;

#define INGRESS_OWNER_DMA (1u << 31)
#define INGRESS_EOF (1u << 30)
#define INGRESS_LENGTH(dw0) (((dw0) >> 12) & 0xfffu)

typedef struct {
    /* Hand a released slot to the DMA. restart = false: the slot is already linked behind the
     * tail, so the DMA only has to re-read the tail's next pointer. restart = true: reset the
     * DMA after an overrun and start it at this slot (the whole ring is armed by then). The
     * callee must also call ingress_update_ready, excluding the interrupt that calls it. */
    void (*rearm)(void *ctx, ingress_desc *slot, bool restart);
    void (*ready)(void *ctx, int level); /* drive the READY pin */
    void *ctx;
} ingress_hw;

typedef struct {
    uint32_t msgs, bytes, nops;             /* accepted messages and payload bytes */
    uint32_t err_short, err_long, err_header, err_crc, err_sync;
    uint32_t lost;                          /* sequence numbers skipped between accepted messages */
    uint32_t seq_back;                      /* accepted messages whose seq went backwards */
    uint32_t overruns;                      /* data arrived with no armed slot (set by hardware) */
    uint32_t restarts;                      /* DMA restarts after an overrun */
    uint32_t ready_falls;                   /* READY high to low transitions */
    uint32_t ts_packets, crc_chain;         /* TS_DATA only; chain the four wire CRC bytes */
} ingress_stats;

typedef struct {
    ingress_desc desc[INGRESS_SLOTS];
    uint32_t head;              /* number of slots released so far; head % SLOTS is the oldest */
    const uint8_t *data;        /* unread payload of the message the consumer holds */
    uint32_t left;
    uint64_t copy_cycles;       /* memcpy windows only; zero in the portable build */
    uint32_t copy_max;
    bool held;                  /* the slot at head is an accepted message not yet released */
    bool in_long;               /* the previous slot ended without end-of-frame */
    bool seq_known;
    uint16_t seq_next;
    volatile bool overrun;      /* set from the DMA interrupt, cleared by the restart */
    unsigned type;             /* 0 accepts both; otherwise lock TS or GCS for the session */
    uint32_t wrong_type;
    int ready;                  /* last level driven on READY */
    ingress_stats st;
    const ingress_hw *hw;
} ingress_ring;

/* buffers: INGRESS_SLOTS * INGRESS_SLOT_BYTES bytes, word aligned, DMA capable.
 * NULL buffers leaves per-slot pointers for the caller to supply. Links slot k to
 * k+1 with every slot armed; start the DMA at desc[0], then call ingress_update_ready. */
void ingress_init(ingress_ring *r, uint8_t *buffers, const ingress_hw *hw);
/* Diagnostic only: repeat four presealed messages, no sender CPU, SPI DMA or ISR. */
void ingress_replay_fill(ingress_ring *r);
void ingress_replay_release(ingress_ring *r, ingress_desc *slot);
/* Copy up to n payload bytes of accepted messages, in order; returns the count (0 if none).
 * Releases each slot as soon as its payload has been copied. */
size_t ingress_read(ingress_ring *r, uint8_t *dst, size_t n);
/* Zero-copy alternative: the payload of the next accepted message, valid until the next call. */
const uint8_t *ingress_next(ingress_ring *r, size_t *len, unsigned *type);
/* Count armed slots and drive READY. Called from the DMA end-of-frame interrupt and by rearm. */
void ingress_update_ready(ingress_ring *r);

#endif
