#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "samples.h"

static uint32_t bank[RX_BANK_PAIRS];
static uint32_t raw[RX_BANK_PAIRS + 4U];
static uint8_t *packed = (uint8_t *)raw;

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
            memset(raw, 0x6d, sizeof raw);
            rx_copy(bank, 0, 1024, raw + offset);
            assert(!memcmp(raw + offset, bank, 1024U * sizeof *raw));
            uint8_t *bytes = (uint8_t *)(raw + offset);
            uint8_t tail = bytes[2048];
            assert(rx_compact(raw + offset, 1024));
            for (int q = -512; q < 512; ++q) {
                assert(bytes[2 * (q + 512)] == (uint8_t)divide4(i));
                assert(bytes[1 + 2 * (q + 512)] == (uint8_t)divide4(q));
            }
            if (offset) assert(raw[offset - 1] == 0x6d6d6d6dU);
            assert(bytes[2048] == tail);
            assert(raw[offset + 1024] == 0x6d6d6d6dU);
        }
    }
    const uint8_t positive_tone[] = {100, 0, 0, 100, 156, 0, 0, 156};
    bank[16382] = word(400, 0);
    bank[16383] = word(0, 400);
    bank[0] = word(-400, 0);
    bank[1] = word(0, -400);
    rx_copy(bank, 16382, 4, raw);
    assert(rx_compact(raw, 4));
    assert(!memcmp(packed, positive_tone, sizeof positive_tone));
    bank[0] = RX_SENTINEL;
    rx_copy(bank, 16382, 4, raw);
    assert(!rx_compact(raw, 4));
    bank[0] = word(-400, 0);
    bank[1] = RX_SENTINEL;
    rx_copy(bank, 16382, 4, raw);
    assert(!rx_compact(raw, 4));
    for (unsigned n = 0; n < RX_BANK_PAIRS; ++n) raw[n] = word(n, -n);
    raw[RX_BANK_PAIRS - 1] = RX_SENTINEL;
    assert(!rx_compact(raw, RX_BANK_PAIRS));
    for (unsigned n = 0; n < RX_BANK_PAIRS; ++n) bank[n] = 0x9e3779b9U * (n + 1U);
    raw[12288] = 0x6d6d6d6dU;
    rx_copy(bank, 16377, 12288, raw);
    for (unsigned n = 0; n < 12288; ++n)
        assert(raw[n] == bank[(16377 + n) & RX_BANK_MASK]);
    assert(raw[12288] == 0x6d6d6d6dU);
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
            rx_copy(bank, first, got, raw);
            for (unsigned n = 0; n < count; ++n)
                assert(raw[n] == bank[(start + n) & RX_BANK_MASK]);
            assert(rx_compact(raw, got));
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
    puts("raw copy, in-place signed10 compaction, wrapping spans and fault rejection pass");
}
