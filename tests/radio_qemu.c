// radio_s3's dry run (the bank sequence of every radio run) in QEMU, at both DAC rates' bank
// geometries. No Wi-Fi, PHY, DAC or bank-ownership register is touched. Each sample word is a
// hash of its index, so the sink can check that every played sample is the right one, in order,
// and the dry run itself compares both banks over each switch's lead and tail.
// Build and run: tests/radio_qemu/README.md. Check: python3 tests/test_radio.py <log>.

#include <inttypes.h>
#include <stdio.h>

#include "radio_s3.h"

static uint32_t word(uint64_t n)
{
    uint32_t h = (uint32_t)n * 2654435761u ^ (uint32_t)(n >> 32);
    return (h ^ h >> 13) & 0xfffff;
}

static esp_err_t fill(uint32_t *dst, uint64_t first, size_t count, void *ctx)
{
    (void)ctx;
    for (size_t i = 0; i < count; ++i) dst[i] = word(first + i);
    return ESP_OK;
}

typedef struct {
    uint64_t next;
    uint32_t calls, bad;
} played_t;

static esp_err_t sink(const uint32_t *bank, uint64_t first, size_t count, void *ctx)
{
    played_t *p = ctx;
    p->bad += first != p->next;
    for (size_t i = 0; i < count; ++i) p->bad += bank[(first + i) % RADIO_S3_BANK_WORDS] != word(first + i);
    p->next = first + count;
    p->calls++;
    return ESP_OK;
}

void app_main(void)
{
    static const uint32_t rates[] = {RADIO_S3_DAC_RATE, RADIO_S3_DAC_RATE_80M};
    static const uint64_t lengths[] = {1, 15740, 1000003};
    for (unsigned r = 0; r < 2; ++r) {
        for (unsigned l = 0; l < 3; ++l) {
            played_t p = {0};
            radio_s3_dry_stats_t s;
            const esp_err_t err = radio_s3_dry_run_rate(fill, NULL, lengths[l], rates[r], sink, &p, &s);
            printf("@{\"test\":\"dry\",\"rate\":%" PRIu32 ",\"samples\":%" PRIu64 ",\"status\":%d,"
                   "\"played\":%" PRIu64 ",\"sink_calls\":%" PRIu32 ",\"bad\":%" PRIu32
                   ",\"fills\":%" PRIu32 ",\"overlap_words\":%" PRIu32
                   ",\"overlap_mismatches\":%" PRIu32 "}\n", rates[r], lengths[l], (int)err,
                   p.next, p.calls, p.bad, s.fills, s.overlap_words, s.overlap_mismatches);
        }
    }
    radio_s3_dry_stats_t s;
    played_t p = {0};
    printf("@{\"test\":\"bad_rate\",\"status\":%d}\n",
           (int)radio_s3_dry_run_rate(fill, NULL, 100, 20000000, sink, &p, &s));
    printf("QEMU-END\n");
}
