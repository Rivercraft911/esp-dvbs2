// esp-dvbs2 firmware. Line commands arrive over the chip's USB port; every result is one JSON
// object on a line starting with '@', so host/board.py can separate results from log output.
// Each command ends with {"cmd":...,"done":true}.
//
//   info       chip, clocks, memory, firmware version and stored callsign
//   callsign [CALL|clear]  read, store or erase the station callsign in NVS (station_commands.c)
//   iddump [CALL] [40|80]  the CW ID's bank samples at 10 kS/s over USB at a 40 or 80 MS/s DAC, no radio
//   membench   memory and bank-fill timing (membench.c)
//   tx <integer MHz> <tone|two|tag|...> <1..60 seconds> [0..17 gain, default 0] [40|80 MS/s]
//              (gap: a 128 tone with every bank's lead and tail silenced, the switch probe;
//              at 80 MS/s every test signal runs at twice its frequency)
//   dvb <MHz> <modcod> [5m|10m|20m|26m667] <1..60 seconds> [0..17 gain] [16..508 peak]  DVB-S2 on air (dvbtx.c)
//   dvbdump <modcod> [5m|10m|20m|26m667] <frames> [peak] [decimation] [stall ms]  USB samples, no radio
//   dvbpace <modcod> [5m|10m|20m|26m667] <seconds> [peak] [stall ms]  paced simulation, no radio
//              (20m and 26m667 use 80 MS/s; 26m667 is QPSK only)
//   wifimem [<modcod> [5m|10m|20m|26m667]]  Wi-Fi start-up heap next to that transmitter's memory;
//              starts and stops Wi-Fi like dvb but never keys, retunes or plays (dvbtx.c)
//   fecbench [plain|pie]  normal-frame encoder correctness and stage timing (fecbench.c)
//   usbrx N    receive N bytes over USB as fast as the host sends them; report the rate

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "dvbtx.h"
#include "spi_input.h"
#include "fecbench.h"
#include "membench.h"
#include "radio_s3.h"
#include "radio_signals.h"
#include "shaper.h"
#include "station_commands.h"
#include "station_store.h"

// tx's test signals borrow the shaping table's internal store: commands run one at a time, and
// every DVB command rebuilds its table. This keeps 13.5 KB of static RAM free for the heap.
_Static_assert(sizeof(radio_signal_t) <= sizeof shaper_store, "tx signal must fit the store");
static radio_signal_t *const tx_signal = (radio_signal_t *)shaper_store;

static IRAM_ATTR esp_err_t fill_signal(uint32_t *dst, uint64_t first, size_t count, void *ctx)
{
    radio_signal_fill(dst, first, count, ctx);
    return ESP_OK;
}

static bool number(const char *text, unsigned lo, unsigned hi, uint32_t *out)
{
    if (!text || text[0] < '0' || text[0] > '9') {
        return false;
    }
    errno = 0;
    char *end;
    unsigned long n = strtoul(text, &end, 10);
    if (errno || *end || n < lo || n > hi) {
        return false;
    }
    *out = n;
    return true;
}

// QPSK defaults to 1 Msym/s. Named rates select one measured kernel per modulation.
static bool tx_mode(const char *code, char **count, char **save, uint32_t *modcod,
                     uint32_t *sps, uint32_t *rate)
{
    *sps = 40;
    *rate = RADIO_S3_DAC_RATE;
    if (!number(code, 4, 17, modcod)) return false;
    const bool named = *count && (!strcmp(*count, "5m") || !strcmp(*count, "10m") ||
                                  !strcmp(*count, "20m") || !strcmp(*count, "26m667"));
    if (*modcod >= 12 && !named) return false;
    if (named) {
        if (!strcmp(*count, "26m667") && *modcod >= 12) return false;
        *sps = !strcmp(*count, "5m") ? 8 : !strcmp(*count, "26m667") ? 3 : 4;
        *rate = !strcmp(*count, "20m") || *sps == 3 ? RADIO_S3_DAC_RATE_80M : RADIO_S3_DAC_RATE;
        *count = strtok_r(NULL, " \t", save);
    }
    return true;
}

static void cmd_dvb(char *args)
{
    if (!station_require("dvb")) {
        return;
    }
    char *save, *mhz = strtok_r(args, " \t", &save);
    char *code = strtok_r(NULL, " \t", &save);
    char *duration = strtok_r(NULL, " \t", &save);
    uint32_t modcod, sps, rate;
    const bool mode_ok = tx_mode(code, &duration, &save, &modcod, &sps, &rate);
    char *level = strtok_r(NULL, " \t", &save);
    char *peak = strtok_r(NULL, " \t", &save);
    char *extra = strtok_r(NULL, " \t", &save);
    uint32_t freq, seconds, gain = RADIO_S3_GAIN_DEFAULT, top = 128;
    if (!number(mhz, 2390, 2450, &freq) || !mode_ok ||
        !number(duration, 1, 60, &seconds) ||
        (level && !number(level, 0, RADIO_S3_GAIN_MAX, &gain)) ||
        (peak && !number(peak, 16, 508, &top)) || extra) {
        printf("@{\"cmd\":\"dvb\",\"error\":\"usage: dvb <2390..2450 MHz> <modcod 4..17> [5m|10m|20m|26m667] "
               "<1..60 seconds> [0..17 gain] [16..508 peak]\"}\n");
        return;
    }
    dvbtx_run(freq, modcod, seconds, gain, top, sps, rate);
}

static void cmd_dvbdump(char *args)
{
    char *save, *code = strtok_r(args, " \t", &save);
    char *count = strtok_r(NULL, " \t", &save);
    uint32_t modcod, sps, rate;
    const bool mode_ok = tx_mode(code, &count, &save, &modcod, &sps, &rate);
    char *peak = strtok_r(NULL, " \t", &save);
    char *step = strtok_r(NULL, " \t", &save);
    char *pause = strtok_r(NULL, " \t", &save);
    char *extra = strtok_r(NULL, " \t", &save);
    uint32_t frames, top = 128, decim = sps == 40 ? 10 : sps < 4 ? 1 : sps / 4, stall = 0;
    if (!mode_ok || !number(count, 1, 256, &frames) ||
        (peak && !number(peak, 16, 508, &top)) || (step && !number(step, 1, 20, &decim)) ||
        (sps % decim || decim > sps / 2) || (pause && !number(pause, 0, 2000, &stall)) || extra) {
        printf("@{\"cmd\":\"dvbdump\",\"error\":\"usage: dvbdump <modcod 4..17> [5m|10m|20m|26m667] <1..256 frames> "
               "[16..508 peak] [decimation dividing sps, at most sps/2] "
               "[0..2000 ms encoder stall]\"}\n");
        return;
    }
    dvbtx_dump(modcod, frames, top, decim, stall, sps, rate);
}

static void cmd_dvbpace(char *args)
{
    char *save, *code = strtok_r(args, " \t", &save);
    char *duration = strtok_r(NULL, " \t", &save);
    uint32_t modcod, sps, rate;
    const bool mode_ok = tx_mode(code, &duration, &save, &modcod, &sps, &rate);
    char *peak = strtok_r(NULL, " \t", &save);
    char *pause = strtok_r(NULL, " \t", &save);
    char *extra = strtok_r(NULL, " \t", &save);
    uint32_t seconds, top = 128, stall = 0;
    if (!mode_ok || !number(duration, 1, 600, &seconds) ||
        (peak && !number(peak, 16, 508, &top)) || (pause && !number(pause, 0, 2000, &stall)) ||
        extra) {
        printf("@{\"cmd\":\"dvbpace\",\"error\":\"usage: dvbpace <modcod 4..17> [5m|10m|20m|26m667] <1..600 seconds> "
               "[16..508 peak] [0..2000 ms encoder stall]\"}\n");
        return;
    }
    dvbtx_pace(modcod, seconds, top, stall, sps, rate);
}

static void cmd_wifimem(char *args)
{
    char *save, *code = strtok_r(args, " \t", &save);
    char *rate = strtok_r(NULL, " \t", &save);
    uint32_t modcod = 0, sps = 0, dac_rate = RADIO_S3_DAC_RATE;
    if (code) {
        const bool ok = tx_mode(code, &rate, &save, &modcod, &sps, &dac_rate);
        // QPSK may omit the rate. Nothing may follow the selected rate.
        if (!ok || rate) {
            printf("@{\"cmd\":\"wifimem\",\"error\":\"usage: wifimem [<modcod 4..17> ["
                   "5m|10m|20m|26m667]]\"}\n");
            return;
        }
    }
    dvbtx_wifi(modcod, sps, dac_rate);
}

static void cmd_tx(char *args)
{
    if (!station_require("tx")) {
        return;
    }
    char *save, *mhz = strtok_r(args, " \t", &save);
    char *name = strtok_r(NULL, " \t", &save);
    char *duration = strtok_r(NULL, " \t", &save);
    char *level = strtok_r(NULL, " \t", &save);
    char *msps = strtok_r(NULL, " \t", &save);
    char *extra = strtok_r(NULL, " \t", &save);
    uint32_t freq, seconds, gain = RADIO_S3_GAIN_DEFAULT, rate = 40;
    if (!number(mhz, 2390, 2450, &freq) || !name ||
        !number(duration, 1, 60, &seconds) ||
        (level && !number(level, 0, RADIO_S3_GAIN_MAX, &gain)) ||
        (msps && (!number(msps, 40, 80, &rate) || (rate != 40 && rate != 80))) || extra ||
        !radio_signal_init(tx_signal, name)) {
        printf("@{\"cmd\":\"tx\",\"error\":\"usage: tx <2390..2450 integer MHz> "
               "<tone|tone<1..511>|two|tag|neg|low|ionly|qonly|zero|dci|dcq|gap> <1..60 seconds> [0..17 gain, default 0] "
               "[40|80 MS/s]\"}\n");
        return;
    }
    rate *= 1000000;
    radio_s3_stats_t s = {0};
    radio_s3_set_gaps(!strcmp(name, "gap"));
    const esp_err_t err = radio_s3_run_rate(freq, gain, seconds, rate, fill_signal, tx_signal, &s);
    radio_s3_set_gaps(false);
    printf("@{\"cmd\":\"tx\",\"freq_mhz\":%lu,\"signal\":\"%s\",\"seconds\":%lu,"
           "\"gain\":%lu,\"dac_rate_nominal\":%u,\"bank_words\":%u,\"step_words\":%u,"
           "\"elapsed_samples\":%llu,\"elapsed_cycles\":%llu,\"fills\":%lu,"
           "\"switches\":%lu,\"late_fills\":%lu,\"late_switches\":%lu,"
           "\"max_fill_cycles\":%lu,\"max_switch_late_cycles\":%lu,"
           "\"min_fill_slack_cycles\":%lu,\"fatal_late\":%lu,\"fe_base_index\":%lu,"
           "\"fe_gain_index\":%lu,\"chain\":\"%s\",\"status\":\"%s\"",
           (unsigned long)freq, name, (unsigned long)seconds, (unsigned long)gain,
           (unsigned)rate, RADIO_S3_BANK_WORDS, (unsigned)RADIO_S3_STEP_FOR(rate),
           (unsigned long long)s.elapsed_samples, (unsigned long long)s.elapsed_cycles,
           (unsigned long)s.fills, (unsigned long)s.switches, (unsigned long)s.late_fills,
           (unsigned long)s.late_switches, (unsigned long)s.max_fill_cycles,
           (unsigned long)s.max_switch_late_cycles, (unsigned long)s.min_fill_slack_cycles,
           (unsigned long)s.fatal_late, (unsigned long)s.fe_base_index,
           (unsigned long)s.fe_gain_index, "cwgain", esp_err_to_name(err));
    if (err != ESP_OK) {
        printf(",\"error\":\"%s\"", err == ESP_ERR_INVALID_ARG ?
               "duration too short for start/end ID" : "radio run failed");
    }
    printf(",\"callsign\":\"%s\",\"id_count\":%lu,\"id_samples\":%lu,"
           "\"planned_samples\":%llu}\n", station_store_get(), (unsigned long)s.id_count,
           (unsigned long)s.id_samples, (unsigned long long)s.planned_samples);
}

static void cmd_info(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    const esp_app_desc_t *app = esp_app_get_description();
    printf("@{\"callsign\":\"%s\",\"cmd\":\"info\",\"firmware\":\"%s\",\"idf\":\"%s\",\"cores\":%d,"
           "\"revision\":%d,\"cpu_mhz\":%d,\"psram_bytes\":%u,\"free_internal\":%u,"
           "\"largest_internal\":%u,\"free_psram\":%u,\"uptime_ms\":%lld}\n",
           station_store_get(), app->version, app->idf_ver, chip.cores, chip.revision,
           CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, (unsigned)esp_psram_get_size(),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), esp_timer_get_time() / 1000);
}

// Payload ingress test: read N bytes straight from the USB driver (not through stdio) and time
// them from the first byte to the last. The host sends a seeded pattern; the CRC lets it confirm
// that every byte arrived intact and in order.
static void cmd_usbrx(unsigned total)
{
    uint8_t *buf = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) {
        printf("@{\"cmd\":\"usbrx\",\"error\":\"allocation failed\"}\n");
        return;
    }
    unsigned got = 0;
    uint32_t crc = 0xFFFFFFFFu;
    int64_t first = 0, last = 0;
    while (got < total) {
        const int want = total - got < 4096 ? (int)(total - got) : 4096;
        const int n = usb_serial_jtag_read_bytes(buf, want, pdMS_TO_TICKS(2000));
        if (n <= 0) {
            break;
        }
        last = esp_timer_get_time();
        if (got == 0) {
            first = last;
        }
        for (int i = 0; i < n; ++i) {
            crc ^= buf[i];
            for (int k = 0; k < 8; ++k) {
                crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
            }
        }
        got += (unsigned)n;
    }
    heap_caps_free(buf);
    const double seconds = (last - first) / 1e6;
    printf("@{\"cmd\":\"usbrx\",\"bytes\":%u,\"requested\":%u,\"us\":%lld,\"mbit_per_s\":%.3f,"
           "\"crc32\":%lu}\n", got, total, (long long)(last - first),
           seconds > 0 ? got * 8 / seconds / 1e6 : 0.0, (unsigned long)~crc);
}

void app_main(void)
{
    usb_serial_jtag_driver_config_t usb = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb.rx_buffer_size = 8192;
    usb.tx_buffer_size = 8192;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
    usb_serial_jtag_vfs_use_driver();
    setvbuf(stdin, NULL, _IONBF, 0);

    const esp_err_t storage = station_store_init();
    if (storage != ESP_OK) {
        printf("@{\"cmd\":\"callsign\",\"error\":\"NVS unavailable or invalid callsign\","
               "\"status\":\"%s\"}\n", esp_err_to_name(storage));
    }
    char line[128];
    for (;;) {
        if (!fgets(line, sizeof line, stdin)) {
            vTaskDelay(1);
            continue;
        }
        if (!strchr(line, '\n') && strlen(line) == sizeof line - 1) {
            int ch;
            do {
                ch = fgetc(stdin);
            } while (ch != '\n' && ch != EOF);
            printf("@{\"error\":\"command too long\"}\n@{\"cmd\":\"\",\"done\":true}\n");
            continue;
        }
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') {
            continue;
        }
        if (strcmp(line, "callsign") == 0 || strncmp(line, "callsign ", 9) == 0 ||
            strncmp(line, "callsign\t", 9) == 0) {
            station_command(line + 8);
            strcpy(line, "callsign");
        } else if (strcmp(line, "iddump") == 0 || strncmp(line, "iddump ", 7) == 0) {
            station_dump(line + 6);
            strcpy(line, "iddump");
        } else if (strcmp(line, "info") == 0) {
            cmd_info();
        } else if (strcmp(line, "membench") == 0) {
            membench_run();
        } else if (strcmp(line, "tx") == 0 || strncmp(line, "tx ", 3) == 0 ||
                   strncmp(line, "tx\t", 3) == 0) {
            cmd_tx(line + 2);
            // cmd_tx tokenizes its arguments in place. Keep the completion record stable.
            strcpy(line, "tx");
        } else if (strncmp(line, "spiinput ", 9) == 0 || strncmp(line, "spipins ", 8) == 0 ||
                   strncmp(line, "spisend ", 8) == 0 || strncmp(line, "spirx ", 6) == 0) {
            char *args = strchr(line, ' '); *args++ = 0;
            spi_input_command(line, args);
        } else if (strncmp(line, "dvbdump ", 8) == 0) {
            cmd_dvbdump(line + 8);
            strcpy(line, "dvbdump");
        } else if (strncmp(line, "dvbpace ", 8) == 0) {
            cmd_dvbpace(line + 8);
            strcpy(line, "dvbpace");
        } else if (strncmp(line, "dvb ", 4) == 0) {
            cmd_dvb(line + 4);
            strcpy(line, "dvb");
        } else if (strcmp(line, "fecbench") == 0) {
            fecbench_run("pie");
        } else if (strncmp(line, "fecbench ", 9) == 0) {
            fecbench_run(line + 9);
        } else if (strcmp(line, "wifimem") == 0 || strncmp(line, "wifimem ", 8) == 0) {
            cmd_wifimem(line + 7);
            strcpy(line, "wifimem");
        } else if (strncmp(line, "usbrx ", 6) == 0) {
            cmd_usbrx((unsigned)strtoul(line + 6, NULL, 10));
        } else {
            printf("@{\"error\":\"unknown command\",\"line\":\"%.40s\"}\n", line);
        }
        printf("@{\"cmd\":\"%.40s\",\"done\":true}\n", line);
    }
}
