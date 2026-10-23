// Radio-off replay: a recorded capture, uploaded to PSRAM, runs through the same receiver as
// `rx` (receive.c), with every push timed, and frames stream in rx's binary records.
#include <stdio.h>
#include "replay.h"

#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"

static int8_t *capture;
static size_t pairs;

void replay_upload(size_t bytes)
{
    heap_caps_free(capture);
    capture = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    pairs = 0;
    if (!capture || bytes % 2) {
        printf("@{\"cmd\":\"upload\",\"error\":\"need an even size that fits in PSRAM\"}\n");
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

esp_err_t replay_run(unsigned modcod, unsigned rate, unsigned skip, receive_stats_t *s,
                     capture_stats_t *cs)
{
    if (!pairs) return ESP_ERR_INVALID_STATE;
    return receive_replay(capture, pairs, modcod, rate, skip, s, cs);
}
