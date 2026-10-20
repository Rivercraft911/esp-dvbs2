// Board-specific correction applied at startup and restored after each run.
// Measured in cbc683c: IQ 3/-12, absolute PBUS pair-0 DC 246/264.
#include "radio_calibration.h"
#include "esp_mac.h"
#include "soc/soc.h"
#include <stdio.h>
#include <string.h>

#define IQ_REG 0x6000607cU
#define IQ_MASK 0x1fffU
static const uint32_t dc_reg[2] = {0x6000611cU, 0x60006120U};
static const unsigned dc_shift[2] = {9, 18};
static const unsigned dc_target[2] = {246, 264};
static const uint8_t board_mac[6] = {0x44, 0xb1, 0x76, 0xb0, 0xe5, 0x8c};
extern void rom_pbus_force_test(uint8_t group, uint8_t index, uint16_t value);

static bool selected;
static radio_calibration_stats_t state;
static bool iq_written;
static unsigned dc_written;

void radio_calibration_command(const char *args)
{
    if (!strcmp(args, "off")) selected = false;
    else if (!strcmp(args, "bench2420")) selected = true;
    else {
        printf("@{\"cmd\":\"rfprofile\",\"error\":\"usage: rfprofile off|bench2420\"}\n");
        return;
    }
    printf("@{\"cmd\":\"rfprofile\",\"profile\":\"%s\",\"board\":\"44b176b0e58c\","
           "\"freq_mhz\":2420,\"gain\":0,\"dac_rate\":40000000,"
           "\"iq_gain\":3,\"iq_phase\":-12,\"dc_pair\":0,\"absolute_dc\":[246,264]}\n",
           selected ? "bench2420" : "off");
}

void radio_calibration_attempt(radio_calibration_stats_t *stats, bool keyed)
{
    memset(stats, 0, sizeof *stats);
    stats->requested = keyed && selected;
}

esp_err_t radio_calibration_begin(uint32_t mhz, unsigned gain, uint32_t dac_rate,
                                  radio_calibration_stats_t *stats)
{
    state = *stats;
    iq_written = false;
    dc_written = 0;
    if (!state.requested) return ESP_OK;
    uint8_t mac[6];
    if (mhz != 2420 || gain != 0 || dac_rate != 40000000 ||
        esp_efuse_mac_get_default(mac) != ESP_OK || memcmp(mac, board_mac, sizeof mac)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    state.guarded = true;
    *stats = state;
    return ESP_OK;
}

bool radio_calibration_filter(radio_filter_stats_t *stats)
{
    if (state.requested && !state.guarded) return false;
    if (!radio_filter_apply(stats)) return false;
    if (!state.requested) return true;
    state.iq_saved = REG_READ(IQ_REG);
    for (unsigned i = 0; i < 2; ++i) {
        state.dc_saved[i] = REG_READ(dc_reg[i]);
        const int delta = (int)dc_target[i] - (int)((state.dc_saved[i] >> dc_shift[i]) & 511U);
        // Reject DC changes more than 16 steps from startup calibration.
        if (delta < -16 || delta > 16) return false;
    }
    // Manufacturer manual TX fields: enable bit11, clear source bit12,
    // gain[4:0], signed phase[10:5]. Preserve all unrelated RX/control bits.
    const uint32_t iq = (state.iq_saved & ~IQ_MASK) | 0x800U | 3U | ((64U-12U) << 5);
    iq_written = true;
    REG_WRITE(IQ_REG, iq);
    state.iq_active = REG_READ(IQ_REG);
    if (state.iq_active != iq) return false;
    for (unsigned i = 0; i < 2; ++i) {
        const uint32_t mask = 511U << dc_shift[i];
        const uint32_t expected = (state.dc_saved[i] & ~mask) | (dc_target[i] << dc_shift[i]);
        dc_written |= 1U << i;
        // These words are readbacks. Actual updates require the PBUS strobe.
        rom_pbus_force_test(2+i, 1, dc_target[i]);
        state.dc_active[i] = REG_READ(dc_reg[i]);
        if (state.dc_active[i] != expected) return false;
    }
    state.applied = true;
    return true;
}

bool radio_calibration_restore(void)
{
    if (!iq_written && !dc_written) return true;
    bool ok = true;
    for (unsigned i = 0; i < 2; ++i) {
        if (!(dc_written & (1U << i))) continue;
        const uint32_t mask = 511U << dc_shift[i];
        const uint32_t expected = (REG_READ(dc_reg[i]) & ~mask) | (state.dc_saved[i] & mask);
        rom_pbus_force_test(2+i, 1, (state.dc_saved[i] & mask) >> dc_shift[i]);
        state.dc_restored[i] = REG_READ(dc_reg[i]);
        ok &= state.dc_restored[i] == expected;
    }
    if (iq_written) {
        const uint32_t expected = (REG_READ(IQ_REG) & ~IQ_MASK) | (state.iq_saved & IQ_MASK);
        REG_WRITE(IQ_REG, expected);
        state.iq_restored = REG_READ(IQ_REG);
        ok &= state.iq_restored == expected;
    }
    state.restored = ok;
    iq_written = false;
    dc_written = 0;
    return ok;
}

void radio_calibration_end(radio_calibration_stats_t *stats)
{
    *stats = state;
}

void radio_calibration_print(const radio_calibration_stats_t *stats)
{
    if (!stats->requested) return;
    printf(",\"rfprofile\":{\"name\":\"bench2420\",\"guarded\":%s,\"applied\":%s,"
           "\"restored\":%s,\"iq_gain\":3,\"iq_phase\":-12,\"dc_pair\":0,"
           "\"absolute_dc\":[246,264],\"iq_saved\":%lu,\"iq_active\":%lu,\"iq_restored\":%lu,"
           "\"dc_saved\":[%lu,%lu],\"dc_active\":[%lu,%lu],\"dc_restored\":[%lu,%lu]}",
           stats->guarded ? "true" : "false", stats->applied ? "true" : "false",
           stats->restored ? "true" : "false", (unsigned long)stats->iq_saved,
           (unsigned long)stats->iq_active, (unsigned long)stats->iq_restored,
           (unsigned long)stats->dc_saved[0], (unsigned long)stats->dc_saved[1],
           (unsigned long)stats->dc_active[0], (unsigned long)stats->dc_active[1],
           (unsigned long)stats->dc_restored[0], (unsigned long)stats->dc_restored[1]);
}
