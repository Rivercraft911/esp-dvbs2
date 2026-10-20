// Radio filter selection, derived from the installed S3 ROM instruction trace.
// Register references and RF measurements are linked in docs/prior-art.md.
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
    snapshot(&s->active);
    s->applied = true;
    s->matches = matches(s, &s->active);
    return s->matches;
}

bool radio_filter_finish(radio_filter_stats_t *s)
{
    snapshot(&s->end);
    s->retained = matches(s, &s->end) &&
                  !memcmp(s->active.caps, s->end.caps, sizeof s->end.caps);
    return s->retained;
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
           ",\"applied\":%s,\"matches\":%s,\"retained\":%s,\"before\":",
           names[s->requested], names[s->selected], s->occupied_hz,
           s->applied ? "true" : "false", s->matches ? "true" : "false",
           s->retained ? "true" : "false");
    print_regs(&s->before);
    printf(",\"active\":"); print_regs(&s->active);
    printf(",\"end\":"); print_regs(&s->end);
    printf("}");
}
