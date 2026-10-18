// esp-dvbs2 USB console. Results start with @; each command ends with done:true.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "console.h"

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
#include "usb_input.h"
#include "radio_s3.h"
#include "radio_signals.h"
#include "shaper.h"
#include "station_commands.h"
#include "station_store.h"

// tx's test signals borrow the shaping table's internal store: commands run one at a time, and
// every DVB command rebuilds its table. No separate signal allocation is needed.
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
                                  !strcmp(*count, "20m") || !strcmp(*count, "26m667") || !strcmp(*count, "33m333"));
    if (*modcod >= 12 && !named) return false;
    if (named) {
        *sps = !strcmp(*count, "33m333") ? DVB_SPS_12_5 : !strcmp(*count, "5m") ? 8 : !strcmp(*count, "26m667") ? 3 : 4;
        *rate = !strcmp(*count, "20m") || *sps == 3 || *sps == DVB_SPS_12_5 ? RADIO_S3_DAC_RATE_80M : RADIO_S3_DAC_RATE;
        *count = strtok_r(NULL, " \t", save);
    }
    return true;
}

/* Remove a standalone opt-in token before the existing positional parser. */
static bool take_option(char *args, const char *flag, bool *enabled)
{
    *enabled = false;
    const size_t length = strlen(flag);
    for (char *p = args; *p;) {
        if (*p == ' ' || *p == '\t') { ++p; continue; }
        char *end = p;
        while (*end && *end != ' ' && *end != '\t') ++end;
        if ((size_t)(end - p) == length && !strncmp(p, flag, length)) {
            if (*enabled) return false;
            *enabled = true;
            memset(p, ' ', end - p);
        }
        p = end;
    }
    return true;
}

static void cmd_dvb(char *args)
{
    if (!station_require("dvb")) {
        return;
    }
    bool fixture;
    const bool source_ok = take_option(args,"--fixture",&fixture);
    char *save, *mhz = strtok_r(args, " \t", &save);
    char *code = strtok_r(NULL, " \t", &save);
    char *duration = strtok_r(NULL, " \t", &save);
    uint32_t modcod, sps, rate;
    const bool mode_ok = tx_mode(code, &duration, &save, &modcod, &sps, &rate);
    char *level = strtok_r(NULL, " \t", &save);
    char *peak = strtok_r(NULL, " \t", &save);
    char *extra = strtok_r(NULL, " \t", &save);
    uint32_t freq, seconds, gain = RADIO_S3_GAIN_DEFAULT, top = 128;
    if (!source_ok || !number(mhz, 2390, 2450, &freq) || !mode_ok ||
        (fixture && !(sps == DVB_SPS_12_5 || (sps == 3 && modcod >= 12))) ||
        !number(duration, 1, 60, &seconds) ||
        (level && !number(level, 0, RADIO_S3_GAIN_MAX, &gain)) ||
        (peak && !number(peak, 16, 508, &top)) || extra) {
        printf("@{\"cmd\":\"dvb\",\"error\":\"usage: dvb <2390..2450 MHz> <modcod 4..17> [5m|10m|20m|26m667|33m333] "
               "<1..60 seconds> [0..17 gain] [16..508 peak] [--fixture]\"}\n");
        return;
    }
    dvbtx_run(freq, modcod, seconds, gain, top, sps, rate, fixture);
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
    uint32_t frames, top = 128, decim = sps == 40 ? 10 : sps < 4 || sps == DVB_SPS_12_5 ? 1 : sps / 4, stall = 0;
    if (!mode_ok || !number(count, 1, 256, &frames) ||
        (peak && !number(peak, 16, 508, &top)) || (step && !number(step, 1, 20, &decim)) ||
        (sps == DVB_SPS_12_5 ? decim != 1 : (sps % decim || decim > sps / 2)) || (pause && !number(pause, 0, 2000, &stall)) || extra) {
        printf("@{\"cmd\":\"dvbdump\",\"error\":\"usage: dvbdump <modcod 4..17> [5m|10m|20m|26m667|33m333] <1..256 frames> "
               "[16..508 peak] [decimation: 33m333=1; otherwise divides sps, at most sps/2] "
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
        printf("@{\"cmd\":\"dvbpace\",\"error\":\"usage: dvbpace <modcod 4..17> [5m|10m|20m|26m667|33m333] <1..600 seconds> "
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
                   "5m|10m|20m|26m667|33m333]]\"}\n");
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
               "<tone|tone<1..511>|two|neg|low|ionly|qonly|zero|dci|dcq> <1..60 seconds> [0..17 gain, default 0] "
               "[40|80 MS/s]\"}\n");
        return;
    }
    rate *= 1000000;
    radio_s3_stats_t s = {0};
    const uint32_t bandwidth = tx_signal->bandwidth_40m * (rate / RADIO_S3_DAC_RATE);
    const esp_err_t err = radio_s3_run_bandwidth(freq, gain, seconds, rate, bandwidth,
                                               fill_signal, tx_signal, &s);
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
    radio_filter_print(&s.filter);
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

void app_main(void)
{
    usb_serial_jtag_driver_config_t usb = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb.rx_buffer_size = 8192;
    usb.tx_buffer_size = 8192;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
    usb_serial_jtag_vfs_use_driver();
    static FILE output = FDEV_SETUP_STREAM(console_put, NULL, NULL, _FDEV_SETUP_WRITE);
    stdout = &output;
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
        } else if (strcmp(line, "tx") == 0 || strncmp(line, "tx ", 3) == 0 ||
                   strncmp(line, "tx\t", 3) == 0) {
            cmd_tx(line + 2);
            // cmd_tx tokenizes its arguments in place. Keep the completion record stable.
            strcpy(line, "tx");
        } else if (strncmp(line, "spiinput ", 9) == 0 || strncmp(line, "spipins ", 8) == 0 ||
                   strncmp(line, "spisend ", 8) == 0 || strncmp(line, "spirx ", 6) == 0) {
            char *args = strchr(line, ' '); *args++ = 0;
            spi_input_command(line, args);
        } else if (strncmp(line, "vcm ", 4) == 0) {
            dvbtx_vcm_command(line + 4);
            strcpy(line, "vcm");
        } else if (strncmp(line, "dvbdump ", 8) == 0) {
            cmd_dvbdump(line + 8);
            strcpy(line, "dvbdump");
        } else if (strncmp(line, "dvbpace ", 8) == 0) {
            cmd_dvbpace(line + 8);
            strcpy(line, "dvbpace");
        } else if (strncmp(line, "dvb ", 4) == 0) {
            cmd_dvb(line + 4);
            strcpy(line, "dvb");
        } else if (strcmp(line, "wifimem") == 0 || strncmp(line, "wifimem ", 8) == 0) {
            cmd_wifimem(line + 7);
            strcpy(line, "wifimem");
        } else {
            printf("@{\"error\":\"unknown command\",\"line\":\"%.40s\"}\n", line);
        }
        usb_input_finish();
        printf("@{\"cmd\":\"%.40s\",\"done\":true}\n", line);
    }
}
