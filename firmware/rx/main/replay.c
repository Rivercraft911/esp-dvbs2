// Radio-off replay: a recorded capture, uploaded to PSRAM, runs through the same receiver as
// `rx` (receive.c), with every push timed, and frames stream in rx's binary records.
#include <stdio.h>
#include "replay.h"

#include "driver/usb_serial_jtag.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"

#include "arena.h"

/* The capture lives in the static store (arena.h), which a dump overwrites. */
static int8_t *const capture = arena_store;
static size_t pairs;

void replay_forget(void)
{
    pairs = 0;
}

void replay_upload(size_t bytes)
{
    pairs = 0;
    if (!bytes || bytes % 2 || bytes > ARENA_STORE_BYTES) {
        printf("@{\"cmd\":\"upload\",\"error\":\"need an even size up to %u bytes\"}\n",
               (unsigned)ARENA_STORE_BYTES);
        return;
    }
    printf("@{\"cmd\":\"upload\",\"ready\":true,\"bytes\":%u}\n", (unsigned)bytes);
    fflush(stdout);
    for (size_t got = 0; got < bytes;) {
        int n = usb_serial_jtag_read_bytes(capture + got, bytes - got, portMAX_DELAY);
        if (n > 0) got += n;
    }
    pairs = bytes / 2;
    printf("@{\"cmd\":\"upload\",\"pairs\":%u,\"crc32\":%lu}\n", (unsigned)pairs,
           (unsigned long)esp_rom_crc32_le(0, (const uint8_t *)capture, bytes));
}

esp_err_t replay_run(unsigned modcod, unsigned rate, unsigned skip, const receive_options_t *options,
                     receive_stats_t *s, capture_stats_t *cs)
{
    if (!pairs) return ESP_ERR_INVALID_STATE;
    return receive_replay(capture, pairs, modcod, rate, skip, options, s, cs);
}
