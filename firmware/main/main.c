// esp-dvbs2 firmware. Line commands arrive over the chip's USB port; every result is one JSON
// object on a line starting with '@', so host/board.py can separate results from log output.
// Each command ends with {"cmd":...,"done":true}.
//
//   info       chip, clocks, memory, firmware version and stored callsign
//   callsign [CALL|clear]  read, store or erase the station callsign in NVS (station_commands.c)
//   iddump [CALL]  the CW ID's bank samples at 10 kS/s over USB, no radio
//   membench   memory and bank-fill timing (membench.c)
//   chain <forced|cw|cw40|cwgain>  how tx turns the transmit chain on (default cwgain)
//   tx <integer MHz> <tone|two|tag|...> <1..60 seconds> [0..17 gain, default 0]
//   dvb <MHz> <modcod> [5m|10m for 8PSK] <1..60 seconds> [0..17 gain] [16..508 peak]  DVB-S2 on air (dvbtx.c)
//   dvbdump <modcod> [5m|10m for 8PSK] <frames> [peak] [decimation] [stall ms]  the same samples over USB, no radio
//   dvbpace <modcod> [5m|10m for 8PSK] <seconds> [peak] [stall ms]  the same encoder, fills and deadlines, no radio
//   fecbench [plain|c|pie]  normal-frame encoder correctness and stage timing (fecbench.c)
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
#include "fecbench.h"
#include "membench.h"
#include "radio_s3.h"
#include "radio_signals.h"
#include "station_commands.h"
#include "station_store.h"

static DRAM_ATTR radio_signal_t signal;

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

// 8PSK always names its rate explicitly. Keep every old QPSK positional argument.
static bool tx_mode(const char *code, char **count, char **save, uint32_t *modcod,
                     uint32_t *sps)
{
    *sps = 40;
    if (!number(code, 4, 17, modcod)) {
        return false;
    }
    if (*modcod >= 12) {
        if (!*count || (strcmp(*count, "5m") && strcmp(*count, "10m"))) {
            return false;
        }
        *sps = !strcmp(*count, "5m") ? 8 : 4;
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
    uint32_t modcod, sps;
    const bool mode_ok = tx_mode(code, &duration, &save, &modcod, &sps);
    char *level = strtok_r(NULL, " \t", &save);
    char *peak = strtok_r(NULL, " \t", &save);
    char *extra = strtok_r(NULL, " \t", &save);
    uint32_t freq, seconds, gain = RADIO_S3_GAIN_DEFAULT, top = 128;
    if (!number(mhz, 2390, 2450, &freq) || !mode_ok ||
        !number(duration, 1, 60, &seconds) ||
        (level && !number(level, 0, RADIO_S3_GAIN_MAX, &gain)) ||
        (peak && !number(peak, 16, 508, &top)) || extra) {
        printf("@{\"cmd\":\"dvb\",\"error\":\"usage: dvb <2390..2450 MHz> <modcod 4..11 | 12..17 5m|10m> "
               "<1..60 seconds> [0..17 gain] [16..508 peak]\"}\n");
        return;
    }
    dvbtx_run(freq, modcod, seconds, gain, top, sps);
}

static void cmd_dvbdump(char *args)
{
    char *save, *code = strtok_r(args, " \t", &save);
    char *count = strtok_r(NULL, " \t", &save);
    uint32_t modcod, sps;
    const bool mode_ok = tx_mode(code, &count, &save, &modcod, &sps);
    char *peak = strtok_r(NULL, " \t", &save);
    char *step = strtok_r(NULL, " \t", &save);
    char *pause = strtok_r(NULL, " \t", &save);
    char *extra = strtok_r(NULL, " \t", &save);
    uint32_t frames, top = 128, decim = sps == 40 ? 10 : sps / 4, stall = 0;
    if (!mode_ok || !number(count, 1, 256, &frames) ||
        (peak && !number(peak, 16, 508, &top)) || (step && !number(step, 1, 20, &decim)) ||
        (sps % decim || decim > sps / 2) || (pause && !number(pause, 0, 2000, &stall)) || extra) {
        printf("@{\"cmd\":\"dvbdump\",\"error\":\"usage: dvbdump <modcod 4..11 | 12..17 5m|10m> <1..256 frames> "
               "[16..508 peak] [decimation dividing sps, at most sps/2] "
               "[0..2000 ms encoder stall]\"}\n");
        return;
    }
    dvbtx_dump(modcod, frames, top, decim, stall, sps);
}

static void cmd_dvbpace(char *args)
{
    char *save, *code = strtok_r(args, " \t", &save);
    char *duration = strtok_r(NULL, " \t", &save);
    uint32_t modcod, sps;
    const bool mode_ok = tx_mode(code, &duration, &save, &modcod, &sps);
    char *peak = strtok_r(NULL, " \t", &save);
    char *pause = strtok_r(NULL, " \t", &save);
    char *extra = strtok_r(NULL, " \t", &save);
    uint32_t seconds, top = 128, stall = 0;
    if (!mode_ok || !number(duration, 1, 600, &seconds) ||
        (peak && !number(peak, 16, 508, &top)) || (pause && !number(pause, 0, 2000, &stall)) ||
        extra) {
        printf("@{\"cmd\":\"dvbpace\",\"error\":\"usage: dvbpace <modcod 4..11 | 12..17 5m|10m> <1..600 seconds> "
               "[16..508 peak] [0..2000 ms encoder stall]\"}\n");
        return;
    }
    dvbtx_pace(modcod, seconds, top, stall, sps);
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
    char *extra = strtok_r(NULL, " \t", &save);
    uint32_t freq, seconds, gain = RADIO_S3_GAIN_DEFAULT;
    if (!number(mhz, 2390, 2450, &freq) || !name ||
        !number(duration, 1, 60, &seconds) ||
        (level && !number(level, 0, RADIO_S3_GAIN_MAX, &gain)) || extra ||
        !radio_signal_init(&signal, name)) {
        printf("@{\"cmd\":\"tx\",\"error\":\"usage: tx <2390..2450 integer MHz> "
               "<tone|tone<1..511>|two|tag|neg|low|ionly|qonly|zero|dci|dcq> <1..60 seconds> [0..17 gain, default 0]\"}\n");
        return;
    }
    radio_s3_stats_t s = {0};
    const esp_err_t err = radio_s3_run(freq, gain, seconds, fill_signal, &signal, &s);
    printf("@{\"cmd\":\"tx\",\"freq_mhz\":%lu,\"signal\":\"%s\",\"seconds\":%lu,"
           "\"gain\":%lu,\"dac_rate_nominal\":%u,\"bank_words\":%u,\"step_words\":%u,"
           "\"elapsed_samples\":%llu,\"elapsed_cycles\":%llu,\"fills\":%lu,"
           "\"switches\":%lu,\"late_fills\":%lu,\"late_switches\":%lu,"
           "\"max_fill_cycles\":%lu,\"max_switch_late_cycles\":%lu,"
           "\"min_fill_slack_cycles\":%lu,\"fatal_late\":%lu,\"fe_base_index\":%lu,"
           "\"fe_gain_index\":%lu,\"chain\":\"%s\",\"status\":\"%s\"",
           (unsigned long)freq, name, (unsigned long)seconds, (unsigned long)gain,
           RADIO_S3_DAC_RATE, RADIO_S3_BANK_WORDS, RADIO_S3_STEP_WORDS,
           (unsigned long long)s.elapsed_samples, (unsigned long long)s.elapsed_cycles,
           (unsigned long)s.fills, (unsigned long)s.switches, (unsigned long)s.late_fills,
           (unsigned long)s.late_switches, (unsigned long)s.max_fill_cycles,
           (unsigned long)s.max_switch_late_cycles, (unsigned long)s.min_fill_slack_cycles,
           (unsigned long)s.fatal_late, (unsigned long)s.fe_base_index,
           (unsigned long)s.fe_gain_index, (const char *[]){"forced", "cw", "cw40", "cwgain"}[s.chain & 3], esp_err_to_name(err));
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
    static uint8_t buf[4096];
    unsigned got = 0;
    uint32_t crc = 0xFFFFFFFFu;
    int64_t first = 0, last = 0;
    while (got < total) {
        const int want = total - got < sizeof buf ? (int)(total - got) : (int)sizeof buf;
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
        } else if (strncmp(line, "dvbdump ", 8) == 0) {
            cmd_dvbdump(line + 8);
            strcpy(line, "dvbdump");
        } else if (strncmp(line, "dvbpace ", 8) == 0) {
            cmd_dvbpace(line + 8);
            strcpy(line, "dvbpace");
        } else if (strncmp(line, "dvb ", 4) == 0) {
            cmd_dvb(line + 4);
            strcpy(line, "dvb");
        } else if (strncmp(line, "chain ", 6) == 0) {
            // How later tx runs turn the transmit chain on (radio_s3_set_chain).
            static const char *const modes[] = {"forced", "cw", "cw40", "cwgain"};
            size_t i = 0;
            while (i < 4 && strcmp(line + 6, modes[i])) {
                ++i;
            }
            if (i < 4) {
                radio_s3_set_chain((radio_s3_chain_t)i);
                printf("@{\"cmd\":\"chain\",\"chain\":\"%s\"}\n", modes[i]);
            } else {
                printf("@{\"cmd\":\"chain\",\"error\":\"usage: chain <forced|cw|cw40|cwgain>\"}\n");
            }
        } else if (strcmp(line, "fecbench") == 0) {
            fecbench_run("plain");
        } else if (strncmp(line, "fecbench ", 9) == 0) {
            fecbench_run(line + 9);
        } else if (strncmp(line, "usbrx ", 6) == 0) {
            cmd_usbrx((unsigned)strtoul(line + 6, NULL, 10));
        } else {
            printf("@{\"error\":\"unknown command\",\"line\":\"%.40s\"}\n", line);
        }
        printf("@{\"cmd\":\"%.40s\",\"done\":true}\n", line);
    }
}
