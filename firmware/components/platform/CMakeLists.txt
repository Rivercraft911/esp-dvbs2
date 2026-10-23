# The chip without Wi-Fi, for both applications: radio power-up and run memory accounting.
# esp_wifi only for its Kconfig: periph_ctrl's wifi_module_enable() needs ESP_WIFI_ENABLED.
# No Wi-Fi code is called, so none is linked.
idf_component_register(SRCS "radio_power.c" "run_arena.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_wifi esp_phy esp_hw_support heap)
