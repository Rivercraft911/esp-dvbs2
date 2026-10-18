// Independent PHY lifetime handoff, checked against installed ESP-IDF v6.1
// phy_init.c / phy_common.c. No BT controller.
#include "physolo.h"

#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_ipc.h"
#include "esp_timer.h"
#include "esp_phy_init.h"
#include "esp_private/phy.h"
#include "esp_wifi.h"
#include "soc/interrupt_core0_reg.h"
#include "soc/interrupt_core1_reg.h"
#include "soc/soc.h"
#include "sdkconfig.h"
#include "xtensa/config/core-isa.h"

esp_err_t physolo_available(void)
{
    /* With BT compiled in the borrowed bit would also request BT PLL tracking.
     * This handoff is qualified only for the project's Wi-Fi-only build. */
#if CONFIG_BT_ENABLED || CONFIG_IEEE802154_ENABLED
    return ESP_ERR_NOT_SUPPORTED;
#else
    return phy_get_modem_flag() == 0 ? ESP_OK : ESP_ERR_INVALID_STATE;
#endif
}

esp_err_t physolo_acquire(physolo_t *solo)
{
    if (solo->held || phy_get_modem_flag() != PHY_MODEM_WIFI) return ESP_ERR_INVALID_STATE;
    esp_wifi_bt_power_domain_on();
    esp_phy_modem_init();
    esp_phy_enable(PHY_MODEM_BT);
    solo->held = true;
    return phy_get_modem_flag() == (PHY_MODEM_WIFI | PHY_MODEM_BT) ?
           ESP_OK : ESP_ERR_INVALID_STATE;
}

void physolo_release(physolo_t *solo)
{
    if (!solo->held) return;
    esp_phy_disable(PHY_MODEM_BT);
    esp_phy_modem_deinit();
    esp_wifi_bt_power_domain_off();
    solo->held = false;
}

static void irq_snapshot(void *arg)
{
    uint8_t *routes = arg;
    const uint32_t base = esp_cpu_get_core_id() == 0 ?
        INTERRUPT_CORE0_MAC_INTR_MAP_REG : INTERRUPT_CORE1_MAC_INTR_MAP_REG;
    const uint32_t external = XCHAL_INTTYPE_MASK_EXTERN_EDGE | XCHAL_INTTYPE_MASK_EXTERN_LEVEL |
                              XCHAL_INTTYPE_MASK_NMI;
    const uint32_t enabled = (esp_cpu_intr_get_enabled_mask() | XCHAL_INTTYPE_MASK_NMI) & external;
    *routes = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const unsigned intr = REG_READ(base + 4 * i) & 31U;
        /* IDF's disconnected route 6 is an internal timer. NMI is unmaskable. */
        if (enabled & (1U << intr)) *routes |= 1U << i;
    }
}

void physolo_snapshot_local(radio_s3_session_stats_t *stats)
{
    wifi_mode_t mode;
    stats->at_us = esp_timer_get_time();
    stats->wifi_mode_result = esp_wifi_get_mode(&mode);
    stats->wifi_initialized = stats->wifi_mode_result != ESP_ERR_WIFI_NOT_INIT;
    stats->phy_users = phy_get_modem_flag();
    stats->free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    stats->largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const unsigned core = esp_cpu_get_core_id();
    irq_snapshot(&stats->wifi_irq_routes[core]);
    stats->irq_sampled_cores = 1U << core;
}

esp_err_t physolo_snapshot(radio_s3_session_stats_t *stats)
{
    physolo_snapshot_local(stats);
    const unsigned core = esp_cpu_get_core_id();
    stats->irq_sampled_cores = 3;
    return esp_ipc_call_blocking(core ^ 1U, irq_snapshot, &stats->wifi_irq_routes[core ^ 1U]);
}
