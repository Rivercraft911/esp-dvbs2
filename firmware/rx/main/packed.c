/* Independent checked packing from the S3 TRM v1.8 PIE instruction definitions.
 * Exact SC8 fields are the same as rx_compact; no decimation or sample repair. */
#include "packed.h"
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define IRAM_ATTR
#endif

extern bool rx_pack_scan8(const uint32_t *raw, size_t groups);
extern void rx_pack8(const uint32_t *raw, uint8_t *out, size_t groups);
extern void rx_pack_clear4(uint32_t *raw, size_t groups);

#ifndef __XTENSA__
bool rx_pack_scan8(const uint32_t *raw, size_t groups)
{
    for (size_t n = 0; n < groups * 8; ++n)
        if (raw[n] == RX_SENTINEL) return false;
    return true;
}
void rx_pack8(const uint32_t *raw, uint8_t *out, size_t groups)
{
    for (size_t n = 0; n < groups * 8; ++n) {
        out[2 * n] = (uint8_t)(raw[n] >> 12);
        out[2 * n + 1] = (uint8_t)(raw[n] >> 2);
    }
}
void rx_pack_clear4(uint32_t *raw, size_t groups)
{
    for (size_t n = 0; n < groups * 4; ++n) raw[n] = RX_SENTINEL;
}
#endif

static bool IRAM_ATTR valid(const uint32_t *raw, size_t pairs)
{
    while (pairs && ((uintptr_t)raw & 31U)) {
        if (*raw++ == RX_SENTINEL) return false;
        --pairs;
    }
    size_t groups = pairs / 8;
    if (!rx_pack_scan8(raw, groups)) return false;
    raw += groups * 8;
    for (size_t n = 0; n < pairs % 8; ++n)
        if (raw[n] == RX_SENTINEL) return false;
    return true;
}

static void IRAM_ATTR pack(const uint32_t *raw, size_t pairs, uint8_t *out)
{
    while (pairs && ((uintptr_t)raw & 31U)) {
        uint32_t word = *raw++;
        *out++ = (uint8_t)(word >> 12);
        *out++ = (uint8_t)(word >> 2);
        --pairs;
    }
    size_t groups = pairs / 8;
    rx_pack8(raw, out, groups);
    raw += groups * 8;
    out += groups * 16;
    for (size_t n = 0; n < pairs % 8; ++n) {
        *out++ = (uint8_t)(raw[n] >> 12);
        *out++ = (uint8_t)(raw[n] >> 2);
    }
}

bool IRAM_ATTR rx_pack_bank(const uint32_t *bank, uint16_t first, size_t pairs, uint8_t *out)
{
    if (first >= RX_BANK_PAIRS || pairs > RX_PACK_MAX_PAIRS ||
        ((uintptr_t)bank & 63U) || (((uintptr_t)out ^ (2U * first)) & 15U)) return false;
    size_t split = RX_BANK_PAIRS - first;
    if (split > pairs) split = pairs;
    if (!valid(bank + first, split) || !valid(bank, pairs - split)) return false;
    pack(bank + first, split, out);
    pack(bank, pairs - split, out + 2U * split);
    return true;
}

void IRAM_ATTR rx_pack_clear(uint32_t *bank)
{
    rx_pack_clear4(bank, RX_BANK_PAIRS / 4U);
}

bool IRAM_ATTR rx_pack_copy_plan(uintptr_t source, uintptr_t out, size_t bytes, rx_packed_plan *plan)
{
    if (bytes > 2U * RX_PACK_MAX_PAIRS || ((source ^ out) & 63U)) return false;
    size_t head = (64U - (out & 63U)) & 63U;
    if (head > bytes) head = bytes;
    size_t bulk = (bytes - head) & ~(size_t)63U;
    *plan = (rx_packed_plan){head, bulk, bytes - head - bulk};
    return true;
}
