/* A C model of the PIE TS CRC-8 kernel (firmware/components/dvbs2/ts_pie.S) for host tests: the
 * same memory reads, 14 aligned 16-byte blocks from align_down(p, 16), then the CRC by columns over
 * positions 0..191 of the packet (187..191 meet zero columns). Built with DVBS2_TS_COLUMN_MODEL
 * (fast.c) or RX_TS_COLUMN_MODEL (rx_ts.c), it stands in for the kernel. */
#include <stdint.h>

unsigned ts_crc_model_calls;

uint32_t dvbs2_pie_crc8_packet(const uint8_t *p, const uint8_t *cols)
{
    const volatile uint8_t *first = (const volatile uint8_t *)((uintptr_t)p & ~(uintptr_t)15);
    uint8_t touched = 0, crc = 0;
    for (unsigned i = 0; i < 224; ++i) touched ^= first[i];
    for (unsigned i = 0; i < 192; ++i)
        for (unsigned k = 0; k < 8; ++k)
            if ((p[i] >> k) & 1) crc ^= cols[(i / 16) * 128 + (7 - k) * 16 + i % 16];
    (void)touched;
    ++ts_crc_model_calls;
    return crc;
}
