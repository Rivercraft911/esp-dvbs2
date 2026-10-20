#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "capture.h"
#include "console.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "esp_rom_crc.h"

static uint32_t control[512];
static uint8_t *allocation, *record;
static rx_report last;
static bool pattern, valid, diagnostic;
static unsigned last_mhz, last_gain;

static bool guards_ok(void)
{
    for (unsigned n = 0; n < 16; ++n)
        if (allocation[n] != 0x6d || record[RX_CAPTURE_BYTES + n] != 0x6d) return false;
    return true;
}

static void report(void)
{
    uint32_t rails = 0;
    for (unsigned n = 0; n < 2U * last.retained_pairs; ++n)
        rails += record[n] == 0x80 || record[n] == 0x7f;
    printf("@{\"cmd\":\"%s\",\"status\":\"%s\",\"cleanup_status\":\"%s\",\"complete\":%s,"
           "\"freq_mhz\":%u,\"gain_index\":%u,\"rate_sps\":16000000,"
           "\"requested_pairs\":%u,\"retained_pairs\":%lu,"
           "\"acquired_pairs\":%lu,\"bytes\":%lu,\"crc32\":%lu,\"units\":%lu,"
           "\"late\":%lu,\"overruns\":%lu,\"boundary_errors\":%lu,\"gap_events\":%lu,"
           "\"sentinel_errors\":%lu,\"max_poll_cycles\":%lu,\"max_copy_cycles\":%lu,"
           "\"max_raw_copy_cycles\":%lu,\"max_clear_cycles\":%lu,\"max_copy_pairs\":%lu,"
           "\"compact_cycles\":%lu,"
           "\"max_switch_late_pairs\":%lu,\"engine_cycles\":%lu,\"elapsed_cycles\":%lu,\"gain_refreshes\":%lu,"
           "\"gain_max\":%u,\"guards_ok\":%s,\"sc8_rail_values\":%lu,"
           "\"snapshot_pairs\":%lu,\"snapshot_crc32\":%lu,"
           "\"format\":\"sc8\",\"order\":\"high10=I,low10=Q\",\"scaling\":\"floor(s10/4)\","
           "\"hardware_qualified\":false",
           pattern ? "pattern" : diagnostic ? "probe" : "capture", esp_err_to_name(last.error),
           esp_err_to_name(last.cleanup_error), valid ? "true" : "false",
           last_mhz, last_gain, diagnostic ? 12288U : RX_RECORD_PAIRS,
           (unsigned long)last.retained_pairs, (unsigned long)last.acquired_pairs,
           (unsigned long)(2U * last.retained_pairs), (unsigned long)last.crc32,
           (unsigned long)last.units, (unsigned long)last.late, (unsigned long)last.overruns,
           (unsigned long)last.boundary_errors, (unsigned long)last.gaps,
           (unsigned long)last.sentinel_errors, (unsigned long)last.max_poll_cycles,
           (unsigned long)last.max_copy_cycles, (unsigned long)last.max_raw_copy_cycles,
           (unsigned long)last.max_clear_cycles, (unsigned long)last.max_copy_pairs,
           (unsigned long)last.compact_cycles, (unsigned long)last.max_switch_late_pairs,
           (unsigned long)last.engine_cycles, (unsigned long)last.elapsed_cycles, (unsigned long)last.gain_refreshes,
           last.gain_max, last.guards_ok ? "true" : "false", (unsigned long)rails,
           (unsigned long)last.snapshot_pairs, (unsigned long)last.snapshot_crc32);
    if (diagnostic) {
        printf(",\"probe\":{\"bytes\":%lu,\"before_start\":%lu,\"after_start\":%lu,"
               "\"start_cycles\":%lu,\"before_switch\":%lu,\"after_switch\":%lu,"
               "\"switch_cycles\":%lu,\"before_stop\":%lu,\"after_stop\":%lu,"
               "\"control\":%lu,\"lanes\":%lu,\"bank_start\":%lu,\"bank_switch\":%lu,"
               "\"edges\":%lu,\"first\":%lu,\"end\":%lu}",
               (unsigned long)last.probe.bytes, (unsigned long)last.probe.before_start,
               (unsigned long)last.probe.after_start, (unsigned long)last.probe.start_cycles,
               (unsigned long)last.probe.before_switch, (unsigned long)last.probe.after_switch,
               (unsigned long)last.probe.switch_cycles, (unsigned long)last.probe.before_stop,
               (unsigned long)last.probe.after_stop, (unsigned long)last.probe.control,
               (unsigned long)last.probe.lanes, (unsigned long)last.probe.bank_start,
               (unsigned long)last.probe.bank_switch, (unsigned long)last.probe.edges,
               (unsigned long)last.probe.first, (unsigned long)last.probe.end);
    }
    if (!valid && !(diagnostic && last.error == ESP_OK && last.guards_ok))
        printf(",\"error\":\"capture incomplete or guards failed\"");
    printf("}\n");
}

static void download(const void *data, size_t count, uint32_t crc, const char *cmd)
{
    printf("@{\"cmd\":\"%s\",\"binary_bytes\":%u,\"crc32\":%lu}\n",
           cmd, (unsigned)count, (unsigned long)crc);
    size_t sent = 0;
    while (sent < count) {
        size_t n = count - sent;
        if (n > 4096) n = 4096;
        int got = usb_serial_jtag_write_bytes((const uint8_t *)data + sent, n, portMAX_DELAY);
        if (got > 0) sent += got;
    }
    putchar('\n');
}

void app_main(void)
{
    usb_serial_jtag_driver_config_t usb = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb.rx_buffer_size = usb.tx_buffer_size = 8192;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
    usb_serial_jtag_vfs_use_driver();
    static FILE output = FDEV_SETUP_STREAM(console_put, NULL, NULL, _FDEV_SETUP_WRITE);
    stdout = &output;
    setvbuf(stdin, NULL, _IONBF, 0);
    allocation = heap_caps_malloc(RX_CAPTURE_BYTES + 32U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (allocation) {
        record = allocation + 16;
        memset(allocation, 0x6d, 16);
        memset(record + RX_CAPTURE_BYTES, 0x6d, 16);
    }
    char line[96];
    while (true) {
        if (!fgets(line, sizeof line, stdin)) { vTaskDelay(1); continue; }
        if (!strchr(line, '\n') && strlen(line) == sizeof line - 1) {
            int ch;
            do { ch = fgetc(stdin); } while (ch != '\n' && ch != EOF);
            printf("@{\"error\":\"command too long\"}\n@{\"done\":true}\n");
            continue;
        }
        line[strcspn(line, "\r\n")] = 0;
        char cmd[16], extra;
        unsigned mhz, gain;
        bool capture = sscanf(line, "%15s %u %u %c", cmd, &mhz, &gain, &extra) == 3 &&
                       (!strcmp(cmd, "capture") || !strcmp(cmd, "probe"));
        if (!strcmp(line, "info")) {
            uint8_t mac[6];
            ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
            printf("@{\"cmd\":\"info\",\"app\":\"s3_receive\",\"firmware\":\"%s\","
                   "\"psram_bytes\":%u,\"record_allocated\":%s,\"capture_buffer_bytes\":%u,"
                   "\"free_psram_bytes\":%u,\"tx\":false,"
                   "\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
                   "\"banks\":[\"0x3fcb0000\",\"0x3fcc0000\",\"0x3fcd0000\"]}\n",
                   esp_app_get_description()->version, (unsigned)esp_psram_get_size(),
                   allocation ? "true" : "false", RX_CAPTURE_BYTES,
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        } else if (!strcmp(line, "membench")) {
            valid = false;
            memset(&last, 0, sizeof last);
            rx_memory_report memory;
            esp_err_t status = allocation ? rx_memory(record, &memory) : ESP_ERR_NO_MEM;
            if (allocation) {
                for (unsigned n = 0; n < RX_MEMORY_CASES; ++n) {
                    rx_memory_result *s = &memory.cases[n];
                    if (!s->name) continue;
                    printf("@{\"cmd\":\"membench\",\"case\":\"%s\",\"units\":128,"
                           "\"unit_bytes\":49152,\"chunk_bytes\":4096,\"cpu_hz\":240000000,"
                           "\"min_cycles\":%lu,\"max_cycles\":%lu,\"total_cycles\":%lu,"
                           "\"max_chunk_cycles\":%lu,\"crc32\":%lu,\"expected_crc32\":%lu,"
                           "\"errors\":%lu}\n", s->name, (unsigned long)s->min_cycles,
                           (unsigned long)s->max_cycles, (unsigned long)s->total_cycles,
                           (unsigned long)s->max_chunk_cycles, (unsigned long)s->crc32,
                           (unsigned long)s->expected_crc32, (unsigned long)s->errors);
                }
            }
            printf("@{\"cmd\":\"membench\",\"status\":\"%s\",\"guards_ok\":%s,"
                   "\"timing\":\"sum of twelve IRQ-masked 4 KiB copies; CRC outside\",\"tx\":false}\n",
                   esp_err_to_name(status), allocation && memory.guards_ok && guards_ok() ? "true" : "false");
        } else if (capture || !strcmp(line, "pattern")) {
            valid = false;
            pattern = !capture;
            diagnostic = capture && !strcmp(cmd, "probe");
            last_mhz = pattern ? 0 : mhz;
            last_gain = pattern ? 0 : gain;
            if (!allocation) {
                printf("@{\"error\":\"6.4 MB PSRAM allocation unavailable\"}\n");
            } else {
                if (pattern) rx_pattern(record, &last);
                else if (mhz < 2390 || mhz > 2450 || gain > 127) {
                    memset(&last, 0, sizeof last);
                    last.error = ESP_ERR_INVALID_ARG;
                } else rx_capture(mhz, gain, record, control, &last, diagnostic);
                last.guards_ok = guards_ok();
                valid = !diagnostic && last.error == ESP_OK && last.guards_ok &&
                        last.retained_pairs == RX_RECORD_PAIRS;
                report();
                if (diagnostic && last.probe.bytes)
                    download(record, last.probe.bytes, last.crc32, "probe_banks");
            }
        } else if (!strcmp(line, "download") && valid) {
            download(record, RX_RECORD_BYTES, last.crc32, "download");
        } else if (!strcmp(line, "snapshot") && !pattern && last.snapshot_pairs) {
            download(control, 4U * last.snapshot_pairs, last.snapshot_crc32, "snapshot");
        } else {
            printf("@{\"error\":\"use info, membench, pattern, capture MHz gain, probe MHz gain, download, snapshot\"}\n");
        }
        printf("@{\"done\":true}\n");
    }
}
