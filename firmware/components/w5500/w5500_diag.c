/* The `eth send` diagnostic's payload and send step (w5500.h). Portable C99. */
#include "w5500.h"

#include <string.h>

void w5500_diag_pattern(uint8_t *p, uint32_t seq)
{
    for (unsigned k = 0; k < 7; ++k, p += 188) {
        const uint8_t head[8] = {0x47, 0x01, 0x00, (uint8_t)(0x10 | ((7 * seq + k) & 15)),
                                 (uint8_t)(seq >> 24), (uint8_t)(seq >> 16), (uint8_t)(seq >> 8),
                                 (uint8_t)seq};
        memcpy(p, head, sizeof head);
        memset(p + sizeof head, (int)k, 188 - sizeof head);
    }
}

int w5500_diag_send(w5500 *w, unsigned s, uint8_t *const buf[2], uint32_t seq)
{
    uint8_t *p = buf[seq & 1];
    w5500_diag_pattern(p, seq);
    return w5500_udp_send(w, s, p, W5500_DIAG_BYTES);
}
