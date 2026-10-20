#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "packed.h"

_Alignas(64) static uint32_t storage[RX_BANK_PAIRS + 32U];
static uint32_t saved[RX_BANK_PAIRS + 32U];
static uint32_t *const bank = storage + 16;
_Alignas(64) static uint8_t output[RX_PACK_STAGE_BYTES + 64U];
static uint32_t random_state = 2026;

static uint32_t random_word(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

static uint8_t oracle(unsigned bits)
{
    int value = (int)(bits & 1023U);
    if (value >= 512) value -= 1024;
    int result = value / 4;
    if (value < 0 && value % 4) --result;
    return (uint8_t)result;
}

static void fill(void)
{
    for (unsigned n = 0; n < sizeof storage / sizeof *storage; ++n) {
        storage[n] = random_word();
        if (storage[n] == RX_SENTINEL) ++storage[n];
    }
}

static void check(unsigned first, size_t pairs, unsigned extra_phase)
{
    memcpy(saved, storage, sizeof saved);
    memset(output, 0x6d, sizeof output);
    size_t offset = 64 + ((2U * first) & 15U) + extra_phase;
    assert(rx_pack_bank(bank, (uint16_t)first, pairs, output + offset));
    assert(!memcmp(storage, saved, sizeof saved));
    for (size_t n = 0; n < pairs; ++n) {
        uint32_t word = saved[16 + ((first + n) & RX_BANK_MASK)];
        assert(output[offset + 2 * n] == oracle(word >> 10));
        assert(output[offset + 2 * n + 1] == oracle(word));
    }
    for (size_t n = 0; n < offset; ++n) assert(output[n] == 0x6d);
    for (size_t n = offset + 2 * pairs; n < sizeof output; ++n)
        assert(output[n] == 0x6d);
}

static void packing(void)
{
    fill();
    /* All signed10 I/Q combinations, with metadata and changing wrap/phase. */
    for (unsigned i = 0; i < 1024; ++i) {
        unsigned first = i & 32U ? RX_BANK_PAIRS - 1U - (i & 31U) : i & 31U;
        for (unsigned q = 0; q < 1024; ++q)
            bank[(first + q) & RX_BANK_MASK] = 0x93600000U | (i << 10) | q;
        check(first, 1024, (i & 3U) * 16U);
    }
    const size_t counts[] = {0, 1, 2, 3, 4, 7, 8, 9, 15, 16, 17, 31,
                             2049, 10288, 12288, RX_PACK_MAX_PAIRS};
    for (unsigned phase = 0; phase < 32; ++phase) {
        for (unsigned wrap = 0; wrap < 2; ++wrap) {
            unsigned first = wrap ? RX_BANK_PAIRS - 1U - phase : phase;
            fill();
            for (unsigned c = 0; c < sizeof counts / sizeof *counts; ++c)
                check(first, counts[c], (phase & 3U) * 16U);
        }
    }
    for (unsigned trial = 0; trial < 512; ++trial) {
        fill();
        unsigned first = random_word() & RX_BANK_MASK;
        size_t pairs = random_word() % (RX_PACK_MAX_PAIRS + 1U);
        check(first, pairs, (random_word() & 3U) * 16U);
    }
}

static void reject(unsigned first, size_t pairs, size_t fault)
{
    unsigned at = (first + fault) & RX_BANK_MASK;
    uint32_t before = bank[at];
    bank[at] = RX_SENTINEL;
    memcpy(saved, storage, sizeof saved);
    memset(output, 0x6d, sizeof output);
    assert(!rx_pack_bank(bank, (uint16_t)first, pairs,
                         output + 64 + ((2 * first) & 15U)));
    /* A second wrapped-span fault must leave first-span output untouched. */
    for (unsigned n = 0; n < sizeof output; ++n) assert(output[n] == 0x6d);
    assert(!memcmp(storage, saved, sizeof saved));
    bank[at] = before;
}

static void refusal(void)
{
    const unsigned starts[] = {0, 1, 7, 16353, 16377, 16383};
    for (unsigned c = 0; c < sizeof starts / sizeof *starts; ++c) {
        unsigned first = starts[c];
        fill();
        for (unsigned fault = 0; fault < 67; ++fault) reject(first, 67, fault);
        /* SIMD alignment must not expand validation outside the owned span. */
        bank[(first - 1U) & RX_BANK_MASK] = RX_SENTINEL;
        bank[(first + 67U) & RX_BANK_MASK] = RX_SENTINEL;
        check(first, 67, 0);
        fill();
        const size_t sizes[] = {RX_UNIT_PAIRS, RX_PACK_MAX_PAIRS};
        for (unsigned n = 0; n < sizeof sizes / sizeof *sizes; ++n) {
            size_t pairs = sizes[n];
            const size_t faults[] = {0, 7, 8, 63, 64, pairs / 2, pairs - 1};
            for (unsigned k = 0; k < sizeof faults / sizeof *faults; ++k)
                reject(first, pairs, faults[k]);
        }
    }
    fill();
    memset(output, 0x6d, sizeof output);
    memcpy(saved, storage, sizeof saved);
    assert(!rx_pack_bank(bank, RX_BANK_PAIRS, 1, output));
    assert(!rx_pack_bank(bank, UINT16_MAX, 1, output));
    assert(!rx_pack_bank(bank, 0, RX_PACK_MAX_PAIRS + 1U, output));
    assert(!rx_pack_bank(bank, 0, SIZE_MAX, output));
    assert(!rx_pack_bank(bank + 1, 0, 8, output));
    assert(!rx_pack_bank(bank, 0, 8, output + 1));
    for (unsigned n = 0; n < sizeof output; ++n) assert(output[n] == 0x6d);
    assert(!memcmp(storage, saved, sizeof saved));
    rx_pack_clear(bank);
    for (unsigned n = 0; n < RX_BANK_PAIRS; ++n) assert(bank[n] == RX_SENTINEL);
    assert(!memcmp(storage, saved, 16 * sizeof *storage));
    assert(!memcmp(bank + RX_BANK_PAIRS, saved + 16 + RX_BANK_PAIRS,
                   16 * sizeof *storage));
}

static void plans(void)
{
    const size_t lengths[] = {129, 513, 2049, 4095, 4096, 24576,
                              2U * RX_PACK_MAX_PAIRS - 1U, 2U * RX_PACK_MAX_PAIRS};
    for (uintptr_t phase = 0; phase < 64; ++phase) {
        for (size_t c = 0; c < 129 + sizeof lengths / sizeof *lengths; ++c) {
            size_t bytes = c < 129 ? c : lengths[c - 129];
            uintptr_t src = UINT32_C(0x3fca0000) + phase;
            uintptr_t dst = UINT32_C(0x3c000000) + phase;
            rx_packed_plan p;
            assert(rx_pack_copy_plan(src, dst, bytes, &p));
            assert(p.head + p.bulk + p.tail == bytes);
            assert(p.head <= 63 && p.tail <= 63 && !(p.bulk & 63U));
            if (p.bulk) {
                assert(!((src + p.head) & 63U));
                assert(!((dst + p.head) & 63U));
                assert(p.bulk <= bytes - p.head);
                /* CPU-written head/tail partial lines do not intersect any
                 * line that the DMA bulk will invalidate/write. */
                if (p.head) assert((dst >> 6) < ((dst + p.head) >> 6));
                if (p.tail) assert(((dst + bytes - 1) >> 6) >=
                                    ((dst + p.head + p.bulk) >> 6));
            }
            if (p.head < bytes) assert(!((dst + p.head) & 63U));
        }
        rx_packed_plan p;
        assert(!rx_pack_copy_plan(UINT32_C(0x3fca0000) + phase,
                                  UINT32_C(0x3c000000) + (phase ^ 1U), 128, &p));
        assert(!rx_pack_copy_plan(UINT32_C(0x3fca0000) + phase,
                                  UINT32_C(0x3c000000) + phase,
                                  2U * RX_PACK_MAX_PAIRS + 1U, &p));
        assert(!rx_pack_copy_plan(UINT32_C(0x3fca0000), UINT32_C(0x3c000000),
                                  SIZE_MAX, &p));
    }
}

int main(void)
{
    assert(RX_PACK_MAX_PAIRS == 14288 && RX_PACK_STAGE_BYTES == 28768);
    packing();
    refusal();
    plans();
    puts("packed RX: signed10 oracle, wrap/phase/bounds, atomic sentinel refusal, guards and DMA cacheline geometry pass");
}
