// Radio-off replay: a recorded capture runs through the same demodulator and FEC as `rx`,
// with every push timed, and frames stream in rx's binary records.
#include <stdio.h>
#include <string.h>
#include "replay.h"

#include "driver/usb_serial_jtag.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"

#include "dvbs2rx.h"
#include "receive.h"

#define CHUNK 6144U     /* words per push; the largest internal block holds 24 KiB */

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

esp_err_t replay_run(unsigned modcod, unsigned skip, replay_stats_t *s)
{
    memset(s, 0, sizeof *s);
    if (!pairs) return ESP_ERR_INVALID_STATE;
    const unsigned internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    rx_config_t config = {.modcod = modcod, .cfo_hint_hz = -10000, .cfo_span_hz = 30000,
                          .carrier_khz = 2420000};
    rx_demod_t *d = heap_caps_aligned_alloc(16, rx_demod_size(), internal);
    rx_fec_t *f = heap_caps_aligned_alloc(16, rx_fec_size(modcod), internal);
    void *work = heap_caps_aligned_alloc(16, rx_fec_work_size(modcod), MALLOC_CAP_SPIRAM);
    uint32_t *words = heap_caps_aligned_alloc(16, 4 * CHUNK, internal);
    rx_frame_t frame = {.hard = heap_caps_aligned_alloc(16, RX_HARD_BYTES, internal),
                        .llr = heap_caps_aligned_alloc(16, RX_SHORT_BITS, MALLOC_CAP_SPIRAM)};
    esp_err_t error = ESP_ERR_NO_MEM;
    if (!d || !f || !work || !words || !frame.hard || !frame.llr) goto done;
    error = ESP_ERR_INVALID_ARG;
    if (rx_demod_init(d, &config) || rx_fec_init(f, modcod, work)) goto done;
    printf("@{\"cmd\":\"replay\",\"stream\":\"binary\",\"record_bytes\":%u}\n",
           (unsigned)sizeof(receive_record_t));
    fflush(stdout);
    for (size_t first = 0, unit = 1; first < pairs; first += CHUNK, ++unit) {
        size_t n = pairs - first < CHUNK ? pairs - first : CHUNK;
        if (skip && unit % skip == 0) continue;
        /* The dump word's I bits 19:12 and Q bits 9:2 are exactly what was recorded. */
        for (size_t k = 0; k < n; ++k)
            words[k] = (uint32_t)(uint8_t)capture[2 * (first + k)] << 12 |
                       (uint32_t)(uint8_t)capture[2 * (first + k) + 1] << 2;
        uint32_t t = esp_cpu_get_cycle_count();
        size_t got = rx_demod_push(d, words, first, n, &frame, 1);
        uint32_t push = esp_cpu_get_cycle_count() - t;
        s->push_cycles += push;
        if (push > s->max_push_cycles) { s->max_push_cycles = push; s->max_push_at = first; }
        if (s->pushes < REPLAY_LOG) s->log[s->pushes] = push;
        ++s->pushes;
        if (!got) continue;
        const uint8_t *payload = NULL;
        rx_fec_stats_t fec = {0};
        t = esp_cpu_get_cycle_count();
        int length = rx_fec_frame(f, &frame, &payload, &fec);
        uint32_t cycles = esp_cpu_get_cycle_count() - t;
        s->fec_cycles += cycles;
        if (cycles > s->max_fec_cycles) s->max_fec_cycles = cycles;
        ++s->frames;
        length >= 0 ? ++s->good : ++s->failed;
        receive_record_t rec = {RECEIVE_MAGIC, frame.sequence, frame.cfo_hz, frame.mer_cdb,
                                (int16_t)length, fec.iterations, fec.failed_checks};
        usb_serial_jtag_write_bytes(&rec, sizeof rec, portMAX_DELAY);
        if (length > 0) usb_serial_jtag_write_bytes(payload, length, portMAX_DELAY);
    }
    receive_record_t end = {RECEIVE_MAGIC, 0, 0, 0, RECEIVE_END, 0, 0};
    usb_serial_jtag_write_bytes(&end, sizeof end, portMAX_DELAY);
    rx_demod_stats(d, &s->demod);
    error = ESP_OK;
done:
    heap_caps_free(frame.llr);
    heap_caps_free(frame.hard);
    heap_caps_free(words);
    heap_caps_free(work);
    heap_caps_free(f);
    heap_caps_free(d);
    return error;
}
