// esp-dvbs2 firmware. Line commands arrive over the chip's USB port; every result is one JSON
// object on a line starting with '@', so host/board.py can separate results from log output.
// Each command ends with {"cmd":...,"done":true}.
//
//   info       chip, clocks, memory and firmware version
//   membench   memory and bank-fill timing (membench.c)
//   usbrx N    receive N bytes over USB as fast as the host sends them; report the rate

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "membench.h"

static void cmd_info(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    const esp_app_desc_t *app = esp_app_get_description();
    printf("@{\"cmd\":\"info\",\"firmware\":\"%s\",\"idf\":\"%s\",\"cores\":%d,"
           "\"revision\":%d,\"cpu_mhz\":%d,\"psram_bytes\":%u,\"free_internal\":%u,"
           "\"free_psram\":%u,\"uptime_ms\":%lld}\n",
           app->version, app->idf_ver, chip.cores, chip.revision,
           CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, (unsigned)esp_psram_get_size(),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
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
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
    usb_serial_jtag_vfs_use_driver();
    setvbuf(stdin, NULL, _IONBF, 0);

    char line[128];
    for (;;) {
        if (!fgets(line, sizeof line, stdin)) {
            vTaskDelay(1);
            continue;
        }
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') {
            continue;
        }
        if (strcmp(line, "info") == 0) {
            cmd_info();
        } else if (strcmp(line, "membench") == 0) {
            membench_run();
        } else if (strncmp(line, "usbrx ", 6) == 0) {
            cmd_usbrx((unsigned)strtoul(line + 6, NULL, 10));
        } else {
            printf("@{\"error\":\"unknown command\",\"line\":\"%.40s\"}\n", line);
        }
        printf("@{\"cmd\":\"%.40s\",\"done\":true}\n", line);
    }
}
