#include "station_store.h"
#include "station_id.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

static char callsign[STATION_CALL_MAX + 1];
static bool ready;

esp_err_t station_store_init(void)
{
    callsign[0] = 0;
    ready = false;
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        return err;
    }
    nvs_handle_t handle;
    err = nvs_open("station", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    char stored[sizeof callsign], normalized[sizeof callsign];
    size_t size = sizeof stored;
    err = nvs_get_str(handle, "callsign", stored, &size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    } else if (err == ESP_OK) {
        if (!station_callsign(stored, normalized) || strcmp(stored, normalized)) {
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(callsign, stored, size);
    }
    ready = err == ESP_OK;
    return err;
}

const char *station_store_get(void)
{
    return callsign;
}

esp_err_t station_store_set(const char *call)
{
    char normalized[sizeof callsign] = {0};
    if (call && !station_callsign(call, normalized)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ready) {
        return ESP_ERR_INVALID_STATE;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open("station", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        callsign[0] = 0;
        ready = false;
        return err;
    }
    err = call ? nvs_set_str(handle, "callsign", normalized) : nvs_erase_key(handle, "callsign");
    if (!call && err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    // A failed write can have an uncertain durable outcome. Require a fresh
    // successful boot/load before allowing RF again.
    if (err != ESP_OK) {
        callsign[0] = 0;
        ready = false;
    } else {
        strcpy(callsign, normalized);
    }
    return err;
}
