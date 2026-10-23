// The radio without Wi-Fi (radio_power.h).
#include "radio_power.h"

#include <stdbool.h>
#include "esp_phy_init.h"
#include "esp_private/periph_ctrl.h"
#include "esp_private/phy.h"
#include "sdkconfig.h"

/* esp_phy_enable() turns the Wi-Fi and BT clocks on itself for the PHY's first, full calibration
 * and its PLL tracking (ESP-IDF v6.1 phy_init.c, periph_ctrl.c phy_module_enable), as a BLE-only
 * application relies on. With Bluetooth not built, the BT modem bit asks for no periodic PLL
 * tracking. */
esp_err_t radio_power_up(void)
{
#if CONFIG_BT_ENABLED || CONFIG_IEEE802154_ENABLED
    return ESP_ERR_NOT_SUPPORTED;
#else
    static bool up;
    if (up) return ESP_OK;
    if (phy_get_modem_flag() != 0) return ESP_ERR_INVALID_STATE;
    esp_wifi_bt_power_domain_on();
    esp_phy_enable(PHY_MODEM_BT);
    wifi_module_enable();
    up = phy_get_modem_flag() == PHY_MODEM_BT;
    return up ? ESP_OK : ESP_ERR_INVALID_STATE;
#endif
}
