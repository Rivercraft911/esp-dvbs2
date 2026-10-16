#include "station_commands.h"
#include "station_id.h"
#include "station_store.h"
#include "radio_s3.h"
#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

bool station_require(const char *cmd)
{
    if (station_store_get()[0]) {
        return true;
    }
    printf("@{\"cmd\":\"%s\",\"error\":\"no callsign set; use callsign <assigned CALL> before RF\"}\n", cmd);
    return false;
}

void station_command(char *args)
{
    char *save, *call = strtok_r(args, " \t", &save);
    if (call) {
        esp_err_t err = ESP_ERR_INVALID_ARG;
        if (!strtok_r(NULL, " \t", &save)) {
            err = station_store_set(strcmp(call, "clear") ? call : NULL);
        }
        if (err != ESP_OK) {
            printf("@{\"cmd\":\"callsign\",\"error\":\"callsign syntax or NVS write failed\","
                   "\"status\":\"%s\"}\n", esp_err_to_name(err));
            return;
        }
    }
    printf("@{\"cmd\":\"callsign\",\"callsign\":\"%s\"}\n", station_store_get());
}

typedef struct {
    uint8_t bytes[4096];
    size_t used;
    uint32_t crc;
} dump_t;

static void flush(dump_t *d)
{
    d->crc = esp_rom_crc32_le(d->crc, d->bytes, d->used);
    usb_serial_jtag_write_bytes(d->bytes, d->used, portMAX_DELAY);
    d->used = 0;
}

static esp_err_t sink(const uint32_t *bank, uint64_t first, size_t count, void *ctx)
{
    dump_t *d = ctx;
    // 40 MS/s / 4000 = 10 kS/s. +1 MHz aliases exactly to DC. This is
    // intentional envelope verification, NOT an antialiased spectral dump.
    for (uint64_t n = (first + 3999) / 4000 * 4000; n < first + count; n += 4000) {
        uint32_t w = bank[n % RADIO_S3_BANK_WORDS];
        int16_t iq[2] = {(int16_t)((int32_t)(w << 22) >> 22),
                         (int16_t)((int32_t)(w << 12) >> 22)};
        memcpy(d->bytes + d->used, iq, sizeof iq);
        d->used += sizeof iq;
        if (d->used == sizeof d->bytes) {
            flush(d);
        }
    }
    return ESP_OK;
}

void station_dump(char *args)
{
    char *save, *call = strtok_r(args, " \t", &save);
    if (!call) {
        call = (char *)station_store_get();
    }
    char normalized[STATION_CALL_MAX + 1];
    if (strtok_r(NULL, " \t", &save) || !station_callsign(call, normalized)) {
        printf("@{\"cmd\":\"iddump\",\"error\":\"usage: iddump [CALL]; no NVS write, no radio\"}\n");
        return;
    }
    station_id_t *id = heap_caps_calloc(1, sizeof *id, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!id) {
        printf("@{\"cmd\":\"iddump\",\"error\":\"allocation failed\"}\n");
        return;
    }
    station_id_init(id, normalized, 0, NULL, NULL);
    static dump_t d;
    memset(&d, 0, sizeof d);
    printf("@{\"cmd\":\"iddump\",\"binary_bytes\":%lu,\"format\":\"int16 I/Q LE\","
           "\"sample_rate\":10000,\"tone_alias_hz\":0,\"callsign\":\"%s\"}\n",
           (unsigned long)((id->length + 3999) / 4000 * 4), normalized);
    fflush(stdout);
    radio_s3_dry_stats_t s;
    esp_err_t err = radio_s3_dry_run(station_cw_fill, id, id->length, sink, &d, &s);
    if (d.used) {
        flush(&d);
    }
    printf("@{\"cmd\":\"iddump\",\"callsign\":\"%s\",\"wpm\":20,\"tone_hz\":1000000,"
           "\"edge_us\":5000,\"dac_samples\":%lu,\"crc32\":%" PRIu32 ",\"fills\":%" PRIu32 ","
           "\"max_fill_cycles\":%" PRIu32 ",\"overlap_words\":%" PRIu32 ","
           "\"overlap_mismatches\":%" PRIu32 ",\"status\":\"%s\"}\n",
           normalized, (unsigned long)id->length, d.crc, s.fills, s.max_fill_cycles,
           s.overlap_words, s.overlap_mismatches, esp_err_to_name(err));
    heap_caps_free(id);
}
