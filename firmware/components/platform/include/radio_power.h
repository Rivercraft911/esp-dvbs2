#pragma once

#include "esp_err.h"

/* The radio, once at boot and for good, without Wi-Fi: powers the modem domain, enables the PHY
 * under the Bluetooth modem bit (its first, full calibration) and keeps the Wi-Fi baseband
 * clocks on, which the transmitter's DAC and the receiver's dump engine need. ESP_OK once the
 * PHY is held, again on later calls; ESP_ERR_NOT_SUPPORTED if Bluetooth or 802.15.4 is built. */
esp_err_t radio_power_up(void);
