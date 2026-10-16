idf_component_register(SRCS "station_id.c" "station_copy.S" "station_store.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES nvs_flash)
