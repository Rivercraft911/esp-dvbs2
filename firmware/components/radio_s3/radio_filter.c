// Independent implementation from the installed S3 ROM instruction trace.
// Archived docs/wide-filter.md gives ROM addresses and masks; see docs/prior-art.md.
#include "radio_filter.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_private/regi2c_ctrl.h"
#include "soc/soc.h"

extern void rom_bb_bss_cbw40(unsigned int mode);
static const char *const names[] = {"auto", "narrow", "wide"};

radio_filter_mode_t radio_filter_get(void)
{
    return RADIO_FILTER_AUTO;
}

static void snapshot(radio_filter_regs_t *r)
{
    r->analog = REG_READ(0x60006100U);
    r->digital = REG_READ(0x6002600cU);
    r->channel = REG_READ(0x60033400U);
    r->cbw = REG_READ(0x6001d000U);
    r->mac = REG_READ(0x6001cc0cU);
    static const uint8_t regs[] = {4, 5, 6, 7, 12, 13, 14, 15};
    for (unsigned i = 0; i < sizeof regs; ++i) {
        r->caps[i] = regi2c_ctrl_read_reg(0x67, 0, regs[i]);
    }
}

static bool matches(const radio_filter_stats_t *s, const radio_filter_regs_t *r)
{
    const bool wide = s->selected == RADIO_FILTER_WIDE;
    if (s->caps_overridden) {
        for (unsigned i = 0; i < sizeof r->caps; ++i) {
            const uint8_t expected = i < 6 ? s->before.caps[i] : (s->before.caps[i] & 0xc0U) | 2U;
            if (r->caps[i] != expected) return false;
        }
    }
    return (r->analog & 0x003f0000U) == (wide ? 0x00120000U : 0) &&
           (r->digital & 0xcU) == (wide ? 4U : 0) &&
           (r->channel & 3U) == (wide ? 3U : 0) &&
           (!wide || (!(r->cbw & 0x40U) && !(r->mac & 1U)));
}

bool radio_filter_apply(radio_filter_stats_t *s)
{
    s->selected = s->occupied_hz > 20000000U ? RADIO_FILTER_WIDE : RADIO_FILTER_NARROW;
    snapshot(&s->before);
    // CW has already reset the PHY to mode 0. Apply AFTER stop_tx_tone and PLL retune.
    rom_bb_bss_cbw40(s->selected == RADIO_FILTER_WIDE ? 2 : 0);
    s->applied = true;
#if defined(RADIO_TX_LPFB2) && RADIO_TX_LPFB2
    if (s->selected == RADIO_FILTER_WIDE) {
        // Isolated A/B: only TX LPF-B I/Q low six bits, calibrated 10 -> 2.
        // This is not a known HT40 setting or a calibrated bandwidth prediction.
        if ((s->before.caps[6] & 63U) != 10 || (s->before.caps[7] & 63U) != 10)
            return false;
        s->caps_overridden = true;
        regi2c_ctrl_write_reg_mask(0x67, 0, 14, 5, 0, 2);
        regi2c_ctrl_write_reg_mask(0x67, 0, 15, 5, 0, 2);
    }
#endif
    snapshot(&s->active);
    s->matches = matches(s, &s->active);
    return s->matches;
}

bool radio_filter_finish(radio_filter_stats_t *s)
{
    snapshot(&s->end);
    s->retained = matches(s, &s->end) &&
                  !memcmp(s->active.caps, s->end.caps, sizeof s->end.caps);
    s->restored = true;
    if (s->caps_overridden) {
        for (unsigned i = 0; i < 2; ++i)
            regi2c_ctrl_write_reg_mask(0x67, 0, 14+i, 5, 0, s->before.caps[6+i] & 63U);
        for (unsigned i = 0; i < 2; ++i)
            if (regi2c_ctrl_read_reg(0x67, 0, 14+i) != s->before.caps[6+i]) s->restored = false;
    }
    return s->retained && s->restored;
}

static void print_regs(const radio_filter_regs_t *r)
{
    printf("{\"analog\":%" PRIu32 ",\"digital\":%" PRIu32 ",\"channel\":%" PRIu32
           ",\"cbw\":%" PRIu32 ",\"mac\":%" PRIu32 ",\"caps\":[",
           r->analog, r->digital, r->channel, r->cbw, r->mac);
    for (unsigned i = 0; i < sizeof r->caps; ++i) {
        printf(i ? ",%u" : "%u", r->caps[i]);
    }
    printf("]}");
}

void radio_filter_print(const radio_filter_stats_t *s)
{
    printf(",\"filter\":{\"requested\":\"%s\",\"selected\":\"%s\",\"occupied_hz\":%" PRIu32
           ",\"applied\":%s,\"matches\":%s,\"retained\":%s,\"caps_overridden\":%s,\"restored\":%s,\"before\":",
           names[s->requested], names[s->selected], s->occupied_hz,
           s->applied ? "true" : "false", s->matches ? "true" : "false",
           s->retained ? "true" : "false", s->caps_overridden ? "true" : "false",
           s->restored ? "true" : "false");
    print_regs(&s->before);
    printf(",\"active\":"); print_regs(&s->active);
    printf(",\"end\":"); print_regs(&s->end);
    printf("}");
}
