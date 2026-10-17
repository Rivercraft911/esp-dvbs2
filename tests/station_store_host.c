// In-memory NVS fault harness. Compiles the actual station_store.c.
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "station_id.h"
#include "station_store.h"
#include "nvs.h"

static char durable[80], pending[80];
const char *esp_err_to_name(int err) { return err ? "NVS_ERROR" : "ESP_OK"; }
bool station_require(const char *cmd);
void station_command(char *args);
static bool exists, pending_exists;
static int init_error, open_error, read_error, write_error, commit_error;
int nvs_flash_init(void) { return init_error; }
int nvs_open(const char *name, int mode, nvs_handle_t *h)
{
    assert(!strcmp(name, "station") && mode == NVS_READWRITE);
    *h = 1;
    strcpy(pending, durable);
    pending_exists = exists;
    return open_error;
}
void nvs_close(nvs_handle_t h) { assert(h == 1); }
int nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *size)
{
    assert(h == 1 && !strcmp(key, "callsign"));
    if (read_error) return read_error;
    if (!exists) return ESP_ERR_NVS_NOT_FOUND;
    if (*size < strlen(durable) + 1) return 9;
    *size = strlen(durable) + 1;
    strcpy(out, durable);
    return 0;
}
int nvs_set_str(nvs_handle_t h, const char *key, const char *value)
{
    assert(h == 1 && !strcmp(key, "callsign"));
    if (write_error) return write_error;
    strcpy(pending, value);
    pending_exists = true;
    return 0;
}
int nvs_erase_key(nvs_handle_t h, const char *key)
{
    assert(h == 1 && !strcmp(key, "callsign"));
    if (write_error) return write_error;
    if (!pending_exists) return ESP_ERR_NVS_NOT_FOUND;
    pending_exists = false;
    pending[0] = 0;
    return 0;
}
int nvs_commit(nvs_handle_t h)
{
    assert(h == 1);
    if (commit_error) return commit_error;
    strcpy(durable, pending);
    exists = pending_exists;
    return 0;
}
int main(void)
{
    assert(station_store_init() == 0 && !station_store_get()[0]);
    assert(station_store_set("n0call/p") == 0);
    assert(!strcmp(station_store_get(), "N0CALL/P"));
    assert(station_store_init() == 0 && !strcmp(station_store_get(), "N0CALL/P"));
    assert(station_store_set("bad\"call") != 0 && !strcmp(station_store_get(), "N0CALL/P"));
    assert(station_store_set(NULL) == 0 && !station_store_get()[0]);
    assert(station_store_init() == 0 && !station_store_get()[0]);
    assert(station_store_set(NULL) == 0); // clear empty storage
    for (int fault = 0; fault < 6; ++fault) {
        assert(station_store_init() == 0);
        assert(station_store_set("K1ABC") == 0);
        if (fault == 0) init_error = 7;
        if (fault == 1) open_error = 7;
        if (fault == 2) read_error = 7;
        if (fault < 3) {
            assert(station_store_init() != 0 && !station_store_get()[0]);
        } else {
            if (fault == 3) write_error = 7;
            if (fault == 4) commit_error = 7;
            if (fault == 5) open_error = 7;
            assert(station_store_set("W2XYZ") != 0 && !station_store_get()[0]);
        }
        assert(station_store_set("N0CALL") != 0);
        init_error = open_error = read_error = write_error = commit_error = 0;
        assert(station_store_init() == 0 && !strcmp(station_store_get(), "K1ABC"));
    }
    assert(station_store_set(NULL) == 0);
    assert(!station_require("tx") && !station_require("dvb"));
    { char args[] = " n0call/p "; station_command(args); }
    assert(station_require("tx") && !strcmp(station_store_get(), "N0CALL/P"));
    { char args[] = ""; station_command(args); }
    { char args[] = "N1OTHER extra"; station_command(args); }
    assert(!strcmp(station_store_get(), "N0CALL/P"));
    { char args[] = "clear"; station_command(args); }
    assert(!station_store_get()[0] && station_store_init() == 0 && !station_store_get()[0]);
    for (unsigned i = 0; i < 3; ++i) {
        strcpy(durable, (const char *[]){"N0CALL\"", "n0call", "ABCDEFGHIJKLMNOPQRSTUVWXYZ123"}[i]);
        exists = true;
        assert(station_store_init() != 0 && !station_store_get()[0]);
        assert(station_store_set("N0CALL") != 0);
    }
    puts("NVS (host stubs): set/read/reboot/clear, validation, init/open/read/write/commit faults, corrupt/oversized values fail closed");
}
