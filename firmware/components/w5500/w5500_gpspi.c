/* GP-SPI2 and GDMA register encodings (w5500_gpspi.h). Portable C99. */
#include "w5500_gpspi.h"

uint32_t w5500_gpspi_addr(uint16_t offset, uint8_t control)
{
    return (uint32_t)offset << 16 | (uint32_t)control << 8;
}

uint32_t w5500_gpspi_clock(uint32_t src_hz, uint32_t want_hz, uint32_t *actual_hz)
{
    for (uint32_t n = 2; n <= 64; ++n) {
        if ((uint64_t)want_hz * n < src_hz) continue;   /* src / n would exceed want */
        *actual_hz = src_hz / n;
        return (n - 1) << 12 | (n / 2 - 1) << 6 | (n - 1);
    }
    *actual_hz = 0;
    return 0;
}

uint32_t w5500_gdma_dw0(unsigned size, unsigned length, bool eof)
{
    return 1u << 31 | (eof ? 1u << 30 : 0) | (length & 0xFFFu) << 12 | (size & 0xFFFu);
}

void w5500_gpspi_load(volatile uint32_t *w, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i += 4) {
        uint32_t v = 0;
        for (size_t j = 0; j < 4 && i + j < n; ++j) v |= (uint32_t)p[i + j] << (8 * j);
        w[i / 4] = v;
    }
}

void w5500_gpspi_store(uint8_t *p, const volatile uint32_t *w, size_t n)
{
    for (size_t i = 0; i < n; i += 4) {
        const uint32_t v = w[i / 4];
        for (size_t j = 0; j < 4 && i + j < n; ++j) p[i + j] = (uint8_t)(v >> (8 * j));
    }
}
