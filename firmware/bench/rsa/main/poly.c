// Guarded integer products: bit i occupies integer bit spacing*i.
#include "poly.h"
#include <string.h>

void poly_pack(uint32_t *out, const uint32_t *in, unsigned bits, unsigned spacing)
{
    memset(out, 0, ((bits * spacing + 31) / 32) * 4);
    for (unsigned i = 0; i < bits; ++i) {
        unsigned at = i * spacing;
        out[at / 32] |= ((in[i / 32] >> (i % 32)) & 1U) << (at % 32);
    }
}

void poly_extract(uint32_t *out, const uint32_t *in, unsigned bits, unsigned spacing)
{
    memset(out, 0, ((bits + 31) / 32) * 4);
    for (unsigned i = 0; i < bits; ++i) {
        unsigned at = i * spacing;
        out[i / 32] |= ((in[at / 32] >> (at % 32)) & 1U) << (i % 32);
    }
}

void poly_reference(uint32_t *out, const uint32_t *a, const uint32_t *b, unsigned bits)
{
    memset(out, 0, ((2 * bits - 1 + 31) / 32) * 4);
    for (unsigned i = 0; i < bits; ++i) {
        if ((a[i / 32] >> (i % 32)) & 1U) {
            for (unsigned j = 0; j < bits; ++j)
                out[(i + j) / 32] ^= ((b[j / 32] >> (j % 32)) & 1U) << ((i + j) % 32);
        }
    }
}
