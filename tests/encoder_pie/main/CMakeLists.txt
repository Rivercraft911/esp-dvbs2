idf_component_register(SRCS "app.c" "bench.c" INCLUDE_DIRS "../../../firmware/main"
                       PRIV_REQUIRES dvbs2 esp_app_format esp_timer)
