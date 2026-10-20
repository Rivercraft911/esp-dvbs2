#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "samples.h"

_Alignas(64) static uint32_t bank[RX_BANK_PAIRS];
_Alignas(64) static uint32_t raw[RX_BANK_PAIRS + 32U];
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

static void plans(void)
{
    const size_t counts[] = {0, 1, 7, 15, 16, 17, 1023, 12288, 14287, 14288, 16384};
    for (unsigned n = 0; n < RX_BANK_PAIRS; ++n) bank[n] = 0x9e3779b9U * (n + 1U);
    for (unsigned first = 0; first < RX_BANK_PAIRS; first += 17) {
        for (unsigned c = 0; c < sizeof counts / sizeof *counts; ++c) {
            size_t count = counts[c];
            rx_copy_plan p;
            assert(rx_plan_copy(first, count, &p));
            assert(p.head + p.bulk + p.tail == count && p.split <= p.bulk);
            assert((p.bulk & 15U) == 0 && (p.split & 15U) == 0);
            if (count <= 14288)
                assert((p.split * 4U + 4031U) / 4032U +
                       ((p.bulk - p.split) * 4U + 4031U) / 4032U <= 16U);
            memset(raw, 0x6d, sizeof raw);
            uint32_t *out = raw + 16U + (first & 15U);
            unsigned at = (first + p.head) & RX_BANK_MASK;
            if (p.bulk) {
                assert(((uintptr_t)(bank + at) & 63U) == 0);
                assert(((uintptr_t)(out + p.head) & 63U) == 0);
            }
            rx_copy(bank, first, p.head, out);
            memcpy(out + p.head, bank + at, p.split * sizeof *out);
            memcpy(out + p.head + p.split, bank, (p.bulk - p.split) * sizeof *out);
            rx_copy(bank, (at + p.bulk) & RX_BANK_MASK, p.tail, out + p.head + p.bulk);
            for (unsigned n = 0; n < count; ++n)
                assert(out[n] == bank[(first + n) & RX_BANK_MASK]);
            assert(out[-1] == 0x6d6d6d6dU && out[count] == 0x6d6d6d6dU);
        }
    }
    rx_copy_plan p;
    assert(!rx_plan_copy(RX_BANK_PAIRS, 1, &p));
    assert(!rx_plan_copy(0, RX_BANK_PAIRS + 1U, &p));
}

static void tuning(void)
{
    const unsigned rf[] = {2000, 2025, 2067, 2110, 2199, 2200, 2245, 2290, 2420, 2450};
    const uint32_t pll[] = {2400000, 2430000, 2480400, 2532000, 2638800,
                            2200000, 2245000, 2290000, 2420000, 2450000};
    for (unsigned n = 0; n < sizeof rf / sizeof *rf; ++n) {
        rx_lo_plan p;
        assert(rx_plan_lo(rf[n], &p));
        assert(p.rf_khz == rf[n] * 1000U && p.pll_khz == pll[n]);
        assert(p.divided == (n < 5));
        assert((p.divided ? 5U * p.pll_khz / 6U : p.pll_khz) == p.rf_khz);
    }
    rx_lo_plan p;
    assert(!rx_plan_lo(1999, &p) && !rx_plan_lo(2451, &p));
    /* Restore only the saved divider bit, including unrelated bits changed
     * since setup. Every possible byte must survive the set/restore pair. */
    for (unsigned saved = 0; saved < 256; ++saved)
        for (unsigned current = 0; current < 256; ++current) {
            uint8_t restored = rx_lo_value(current, (saved & 0x10U) != 0);
            assert((restored & ~0x10U) == (current & ~0x10U));
            assert((restored & 0x10U) == (saved & 0x10U));
            assert(rx_lo_value(rx_lo_value(saved, false), (saved & 0x10U) != 0) == saved);
            assert(rx_lo_value(rx_lo_value(saved, true), (saved & 0x10U) != 0) == saved);
        }
}

int main(void)
{
    packing();
    boundaries();
    plans();
    tuning();
    puts("raw copy, in-place signed10 compaction, wrapping spans and fault rejection pass");
}
