#include "ingress_pattern.h"
#include <string.h>

void ingress_pv_pattern(uint8_t *p, uint16_t seq)
{
    for (unsigned i = 0; i < 7; ++i, p += PV_TS_PACKET) {
        const unsigned k = 7u * seq + i;
        p[0] = 0x47; p[1] = 1; p[2] = 0; p[3] = 0x10 | (k & 15);
        for (unsigned j = 0; j < 184; ++j) p[4+j] = (k % 112 + j) & 255;
    }
}

void ingress_pattern(uint8_t *p, uint16_t seq, unsigned type)
{
    for (unsigned j = 0; j < 7; ++j, p += PV_TS_PACKET) {
        uint32_t x = (((uint32_t)seq << 8 | j) * 0x9e3779b1u) | 1u;
        for (unsigned k = 0; k < PV_TS_PACKET; k += 4) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            memcpy(p + k, &x, 4);
        }
        if (type == PV_TS) {
            p[0] = 0x47; p[1] = 1; p[2] = 0;
            p[3] = 0x10 | ((seq * 7 + j) & 15);
            p[4] = seq; p[5] = seq >> 8; p[6] = j; p[7] = 0xa5;
        }
    }
}
