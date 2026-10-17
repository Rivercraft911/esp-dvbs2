#pragma once
#include "esp_err.h"

/* NVS errors fail closed. No automatic partition erasure. No flash access in fills. */
esp_err_t station_store_init(void);
const char *station_store_get(void);
esp_err_t station_store_set(const char *call); /* NULL clears */
