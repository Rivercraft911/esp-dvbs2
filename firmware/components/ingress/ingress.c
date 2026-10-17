/* PV message checks and the slot ring between the SPI slave DMA and the encoder (see ingress.h).
 * Portable C99. INGRESS_IRAM places the per-message path in ESP32 instruction RAM, as the
 * end-of-frame interrupt calls ingress_update_ready. Little-endian targets only. */
#include "ingress.h"

#include <string.h>

#ifdef INGRESS_IRAM
#define HOT __attribute__((section(".iram1"), noinline))
#else
#define HOT
#endif

/* Four 256-entry tables with separate base addresses, so each lookup is extract, scale-add and
 * load. Built on first use, 4 KiB in internal RAM. */
static uint32_t crc_t0[256], crc_t1[256], crc_t2[256], crc_t3[256];
static bool crc_ready;
typedef uint32_t __attribute__((may_alias, aligned(4))) word32;

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
        }
        crc_t0[i] = c;
    }
    for (uint32_t i = 0; i < 256; ++i) {
        crc_t1[i] = (crc_t0[i] >> 8) ^ crc_t0[crc_t0[i] & 0xff];
        crc_t2[i] = (crc_t1[i] >> 8) ^ crc_t0[crc_t1[i] & 0xff];
        crc_t3[i] = (crc_t2[i] >> 8) ^ crc_t0[crc_t2[i] & 0xff];
    }
    crc_ready = true;
}

/* Slicing by four: one aligned 32-bit load and four table lookups per word. */
HOT uint32_t pv_crc32(uint32_t crc, const void *data, size_t n)
{
    if (!crc_ready) {
        crc_init();
    }
    const uint8_t *p = data;
    crc = ~crc;
    for (; n && ((uintptr_t)p & 3); --n) {
        crc = crc_t0[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    }
    const word32 *w = (const word32 *)p;
    const size_t words = n / 4;
    for (size_t i = 0; i < words; ++i) {
        crc ^= w[i];
        crc = crc_t3[crc & 0xff] ^ crc_t2[(crc >> 8) & 0xff] ^ crc_t1[(crc >> 16) & 0xff] ^
              crc_t0[crc >> 24];
    }
    p += words * 4;
    for (n &= 3; n; --n) {
        crc = crc_t0[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    }
    return ~crc;
}

static uint32_t get16(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8;
}

static uint32_t get32(const uint8_t *p)
{
    return get16(p) | get16(p + 2) << 16;
}

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xff;
    p[1] = v >> 8 & 0xff;
}

void pv_seal(uint8_t *msg, unsigned type, uint16_t seq, size_t payload_bytes)
{
    if (payload_bytes > PV_PAYLOAD_MAX) return;
    msg[0] = 0x50;
    msg[1] = 0x56;
    msg[2] = 1;
    msg[3] = (uint8_t)type;
    put16(msg + 4, seq);
    put16(msg + 6, (uint32_t)payload_bytes);
    memset(msg + 8, 0, 4);
    memset(msg + PV_HEADER_BYTES + payload_bytes, 0, PV_PAYLOAD_MAX - payload_bytes);
    const uint32_t crc = pv_crc32(0, msg, PV_MSG_BYTES - 4);
    put16(msg + PV_MSG_BYTES - 4, crc & 0xffff);
    put16(msg + PV_MSG_BYTES - 2, crc >> 16);
}

HOT pv_status pv_check(const uint8_t *m, size_t len)
{
    if (len < PV_MSG_BYTES) {
        return PV_SHORT;
    }
    if (len > PV_MSG_BYTES) {
        return PV_LONG;
    }
    if (m[0] != 0x50 || m[1] != 0x56 || m[2] != 1 || get32(m + 8) != 0) {
        return PV_HEADER;
    }
    if (pv_crc32(0, m, PV_MSG_BYTES - 4) != get32(m + PV_MSG_BYTES - 4)) {
        return PV_CRC;
    }
    const uint32_t type = m[3], n = get16(m + 6);
    switch (type) {
    case PV_NOP:
        return n == 0 ? PV_OK : PV_HEADER;
    case PV_GCS:
        return n >= 1 && n <= PV_PAYLOAD_MAX ? PV_OK : PV_HEADER;
    case PV_TS:
        if (n == 0 || n > PV_PAYLOAD_MAX || n % PV_TS_PACKET) {
            return PV_HEADER;
        }
        for (uint32_t k = 0; k < n; k += PV_TS_PACKET) {
            if (m[PV_HEADER_BYTES + k] != 0x47) {
                return PV_SYNC;
            }
        }
        return PV_OK;
    default:
        return PV_HEADER;
    }
}

void ingress_init(ingress_ring *r, uint8_t *buffers, const ingress_hw *hw)
{
    memset(r, 0, sizeof *r);
    r->hw = hw;
    r->ready = -1;
    for (int i = 0; i < INGRESS_SLOTS; ++i) {
        ingress_desc *d = &r->desc[i];
        d->buf = buffers + i * INGRESS_SLOT_BYTES;
        d->next = i + 1 < INGRESS_SLOTS ? &r->desc[i + 1] : NULL;
        d->dw0 = INGRESS_OWNER_DMA | INGRESS_SLOT_BYTES;
    }
    if (!crc_ready) {
        crc_init();
    }
}

HOT void ingress_update_ready(ingress_ring *r)
{
    uint32_t armed = 0;
    for (int i = 0; i < INGRESS_SLOTS; ++i) {
        armed += r->desc[i].dw0 >> 31;
    }
    const int level = !r->overrun && armed >= INGRESS_READY_MARGIN;
    if (level != r->ready) {
        r->st.ready_falls += r->ready == 1;
        r->ready = level;
        r->hw->ready(r->hw->ctx, level);
    }
}

/* Give the slot at head back to the DMA: re-arm its descriptor, link it behind the tail (the
 * slot released before it), then let the hardware continue. */
HOT static void release(ingress_ring *r)
{
    ingress_desc *d = &r->desc[r->head % INGRESS_SLOTS];
    d->next = NULL;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    d->dw0 = INGRESS_OWNER_DMA | INGRESS_SLOT_BYTES;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    r->desc[(r->head + INGRESS_SLOTS - 1) % INGRESS_SLOTS].next = d;
    r->head++;
    r->held = false;
    r->hw->rearm(r->hw->ctx, d, false);
}

/* Make the next accepted message current. Rejected slots are counted and released. */
HOT static bool advance(ingress_ring *r)
{
    for (unsigned scanned = 0; scanned < INGRESS_SLOTS; ++scanned) {
        ingress_desc *d = &r->desc[r->head % INGRESS_SLOTS];
        const uint32_t w = d->dw0;
        if (w & INGRESS_OWNER_DMA) {
            if (r->overrun) {
                /* Drained after an overrun: every slot is armed and linked from here on. */
                r->hw->rearm(r->hw->ctx, d, true);
                r->st.restarts += !r->overrun;
            }
            return false;
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE); /* descriptor before data */
        if (!(w & INGRESS_EOF)) {
            /* The slot filled up before the transaction ended. */
            r->st.err_long += !r->in_long;
            r->in_long = true;
            release(r);
            continue;
        }
        if (r->in_long) {
            r->in_long = false; /* the end of that long transaction */
            release(r);
            continue;
        }
        const uint8_t *m = d->buf;
        switch (pv_check(m, INGRESS_LENGTH(w))) {
        case PV_OK:
            break;
        case PV_SHORT:
            r->st.err_short++;
            release(r);
            continue;
        case PV_LONG:
            r->st.err_long++;
            release(r);
            continue;
        case PV_CRC:
            r->st.err_crc++;
            release(r);
            continue;
        case PV_SYNC:
            r->st.err_sync++;
            release(r);
            continue;
        default:
            r->st.err_header++;
            release(r);
            continue;
        }
        const uint16_t seq = (uint16_t)get16(m + 4);
        if (r->seq_known) {
            const uint16_t gap = (uint16_t)(seq - r->seq_next);
            if (gap < 0x8000) {
                r->st.lost += gap;
            } else {
                r->st.seq_back++;
                release(r);
                continue;
            }
        }
        r->seq_known = true;
        r->seq_next = seq + 1;
        if (m[3] == PV_NOP) {
            r->st.nops++;
            release(r);
            continue;
        }
        if (r->type && m[3] != r->type) {
            r->wrong_type++;
            release(r);
            continue;
        }
        r->data = m + PV_HEADER_BYTES;
        r->left = get16(m + 6);
        r->held = true;
        r->st.msgs++;
        r->st.bytes += r->left;
        return true;
    }
    return false;  /* yield under a continuous flood of invalid messages */
}

HOT size_t ingress_read(ingress_ring *r, uint8_t *dst, size_t n)
{
    size_t done = 0;
    while (done < n) {
        if (!r->left) {
            if (r->held) {
                release(r);
            }
            if (!advance(r)) {
                break;
            }
        }
        const size_t k = n - done < r->left ? n - done : r->left;
        memcpy(dst + done, r->data, k);
        r->data += k;
        r->left -= k;
        done += k;
    }
    if (r->held && !r->left) {
        release(r);
    }
    return done;
}

const uint8_t *ingress_next(ingress_ring *r, size_t *len, unsigned *type)
{
    if (r->held) {
        release(r);
    }
    if (!advance(r)) {
        return NULL;
    }
    const uint8_t *payload = r->data;
    *len = r->left;
    *type = payload[3 - PV_HEADER_BYTES];
    r->left = 0;
    return payload;
}
