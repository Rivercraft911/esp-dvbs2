#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "samples.h"

static uint32_t bank[RX_BANK_PAIRS];
static uint8_t packed[2U * RX_BANK_PAIRS + 2U];

static uint32_t word(int i, int q)
{
    return (((uint32_t)i & 1023U) << 10) | ((uint32_t)q & 1023U);
}

static int divide4(int value)
{
    return value >= 0 ? value / 4 : (value - 3) / 4;
}

static void packing(void)
{
    /* Exhaust every signed10 field combination against a signed arithmetic oracle. */
    for (int i = -512; i < 512; ++i) {
        for (int q = -512; q < 512; ++q) bank[q + 512] = word(i, q);
        for (unsigned offset = 0; offset < 4; ++offset) {
            memset(packed, 0x6d, sizeof packed);
            assert(rx_pack(bank, 0, 1024, packed + offset));
            for (int q = -512; q < 512; ++q) {
                assert(packed[offset + 2 * (q + 512)] == (uint8_t)divide4(i));
                assert(packed[offset + 1 + 2 * (q + 512)] == (uint8_t)divide4(q));
            }
            if (offset) assert(packed[offset - 1] == 0x6d);
            assert(packed[offset + 2048] == 0x6d);
        }
    }
    const uint8_t positive_tone[] = {100, 0, 0, 100, 156, 0, 0, 156};
    bank[16382] = word(400, 0);
    bank[16383] = word(0, 400);
    bank[0] = word(-400, 0);
    bank[1] = word(0, -400);
    assert(rx_pack(bank, 16382, 4, packed));
    assert(!memcmp(packed, positive_tone, sizeof positive_tone));
    bank[0] = RX_SENTINEL;
    assert(!rx_pack(bank, 16382, 4, packed));
    bank[0] = word(-400, 0);
    bank[1] = RX_SENTINEL;
    assert(!rx_pack(bank, 16382, 4, packed));
}

static void boundaries(void)
{
    /* Unit cuts move around all four ring quadrants, crossing index zero. */
    for (unsigned start = 0; start < RX_BANK_PAIRS; start += 17) {
        for (int jitter = -10; jitter <= 10; ++jitter) {
            for (unsigned n = 0; n < RX_BANK_PAIRS; ++n) bank[n] = RX_SENTINEL;
            size_t count = 12288 + jitter;
            unsigned stop = (start + count) & RX_BANK_MASK;
            for (unsigned n = 0; n < count; ++n)
                bank[(start + n) & RX_BANK_MASK] = word(n * 13, n * 29 + 7);
            uint16_t first;
            size_t got;
            rx_edge begin = {(start - 2) & RX_BANK_MASK, (start + 3) & RX_BANK_MASK};
            rx_edge end = {(stop - 2) & RX_BANK_MASK, (stop + 3) & RX_BANK_MASK};
            assert(rx_span(bank, begin, end, &first, &got));
            assert(first == start && got == count);
            assert(rx_pack(bank, first, got, packed));
            for (unsigned n = 0; n < count; ++n) {
                assert(packed[2U * n] == (uint8_t)(((n * 13U) & 1023U) >> 2));
                assert(packed[2U * n + 1U] == (uint8_t)(((n * 29U + 7U) & 1023U) >> 2));
            }
        }
    }
    uint16_t edge;
    assert(!rx_find_edge(bank, (rx_edge){0, 65}, true, &edge));
    for (unsigned n = 0; n < RX_BANK_PAIRS; ++n) bank[n] = RX_SENTINEL;
    assert(!rx_find_edge(bank, (rx_edge){0, 0}, true, &edge));
    /* An isolated sentinel inside a transition window must not hide a hole. */
    for (unsigned n = 100; n < 1000; ++n) bank[n] = 0;
    bank[120] = RX_SENTINEL;
    assert(!rx_find_edge(bank, (rx_edge){100, 100}, true, &edge));
    /* Measured frozen first bank: the true edge lies inside the old77-pair
     * software interval, but that interval must still fail the64-pair bound. */
    for (unsigned n = 0; n < RX_BANK_PAIRS; ++n)
        bank[n] = n < 12348 ? 0x218007fcU : RX_SENTINEL;
    assert(!rx_find_edge(bank, (rx_edge){12290, 12367}, false, &edge));
    assert(rx_find_edge(bank, (rx_edge){12340, 12356}, false, &edge));
    assert(edge == 12348);
    uint16_t first;
    size_t count;
    assert(rx_span(bank, (rx_edge){0, 0}, (rx_edge){12340, 12356}, &first, &count));
    assert(first == 0 && count == 12348);
}

int main(void)
{
    packing();
    boundaries();
    puts("signed10 packing, I/Q orientation, wrapping spans and fault rejection pass");
}
