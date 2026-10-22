#include "kernels.h"

void rx_unpack_c(const uint32_t *words, size_t blocks, int8_t *i, int8_t *q)
{
    for (size_t n = 0; n < blocks * RX_UNPACK_BLOCK; ++n) {
        i[n] = (int8_t)((int32_t)(words[n] << 12) >> 24);  /* I bits 19:12 */
        q[n] = (int8_t)((int32_t)(words[n] << 22) >> 24);  /* Q bits 9:2 */
    }
}

void rx_unpack_span(const uint32_t *words, size_t n, int8_t *i, int8_t *q)
{
    size_t head = (16U - ((uintptr_t)i & 15U)) & 15U;
    if (head > n) head = n;
    for (size_t k = 0; k < head; ++k) {
        i[k] = (int8_t)((int32_t)(words[k] << 12) >> 24);
        q[k] = (int8_t)((int32_t)(words[k] << 22) >> 24);
    }
    size_t blocks = (n - head) / RX_UNPACK_BLOCK, done = head + blocks * RX_UNPACK_BLOCK;
    rx_unpack(words + head, blocks, i + head, q + head);
    for (size_t k = done; k < n; ++k) {
        i[k] = (int8_t)((int32_t)(words[k] << 12) >> 24);
        q[k] = (int8_t)((int32_t)(words[k] << 22) >> 24);
    }
}

void rx_mf_c(const int8_t *i, const int8_t *q, const rx_mf_step_t *steps, size_t n,
             unsigned vectors, int32_t *out)
{
    for (size_t k = 0; k < n; ++k) {
        const int8_t *xi = i + steps[k].base, *xq = q + steps[k].base, *h = steps[k].coef;
        int32_t si = 0, sq = 0;
        for (unsigned j = 0; j < 16 * vectors; ++j) {
            si += xi[j] * h[j];
            sq += xq[j] * h[j];
        }
        out[2 * k] = si;
        out[2 * k + 1] = sq;
    }
}

#if !DVBS2RX_PIE
void rx_unpack(const uint32_t *words, size_t blocks, int8_t *i, int8_t *q)
{
    rx_unpack_c(words, blocks, i, q);
}

void rx_mf(const int8_t *i, const int8_t *q, const rx_mf_step_t *steps, size_t n,
           unsigned vectors, int32_t *out)
{
    rx_mf_c(i, q, steps, n, vectors, out);
}
#endif
