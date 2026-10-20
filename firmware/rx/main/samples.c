/* Independently written from the documented RX word fields and bank geometry.
 * Register/format reference, not imported implementation:
 * https://github.com/cyber-murmel/esp-sdr-zephyr/blob/3214c16a7c9f136414cc5ae9d245ea290c98a6ce/doc/undocumented-registers-esp32s3.md
 * "Sample word formats": high signed10 = I, low signed10 = Q.
 */
#include "samples.h"
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define IRAM_ATTR
#endif

bool IRAM_ATTR rx_find_edge(const uint32_t *bank, rx_edge bracket, bool entering,
                            uint16_t *index)
{
    unsigned width = (bracket.after - bracket.before) & RX_BANK_MASK;
    if (width > RX_EDGE_GUARD) return false;
    unsigned at = (bracket.before - RX_EDGE_GUARD) & RX_BANK_MASK;
    bool previous = bank[(at - 1U) & RX_BANK_MASK] != RX_SENTINEL;
    unsigned found = 0;
    for (unsigned n = 0; n <= width + 2U * RX_EDGE_GUARD; ++n) {
        bool present = bank[at] != RX_SENTINEL;
        if (present != previous) {
            if (present != entering) return false;
            *index = at;
            ++found;
        }
        previous = present;
        at = (at + 1U) & RX_BANK_MASK;
    }
    return found == 1;
}

bool IRAM_ATTR rx_span(const uint32_t *bank, rx_edge begin, rx_edge end,
                       uint16_t *first, size_t *pairs)
{
    uint16_t last;
    if (!rx_find_edge(bank, begin, true, first) || !rx_find_edge(bank, end, false, &last))
        return false;
    *pairs = (last - *first) & RX_BANK_MASK;
    return *pairs > 0 && *pairs < RX_BANK_PAIRS - 2U * RX_EDGE_GUARD;
}

bool IRAM_ATTR rx_pack(const uint32_t *bank, uint16_t first, size_t pairs, uint8_t *out)
{
    while (pairs) {
        size_t count = RX_BANK_PAIRS - first;
        if (count > pairs) count = pairs;
        const uint32_t *src = bank + first, *end = src + count;
        while (src != end) {
            uint32_t word = *src++;
            if (word == RX_SENTINEL) return false;
            /* Discard the low two bits of each signed10 field: floor(value / 4).
             * Keep I,Q byte order explicit; do not reuse TX packing. */
            *out++ = (uint8_t)(word >> 12);
            *out++ = (uint8_t)(word >> 2);
        }
        pairs -= count;
        first = 0;
    }
    return true;
}
