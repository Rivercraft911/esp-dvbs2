// esp-dvbs2 firmware. Line commands arrive over the chip's USB port; every result is one JSON
// object on a line starting with '@', so host/board.py can separate results from log output.
// Each command ends with {"cmd":...,"done":true}.
//
//   info       chip, clocks, memory and firmware version
//   membench   memory and bank-fill timing (membench.c)

#include <stdio.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
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
           "\"free_psram\":%u}\n",
           app->version, app->idf_ver, chip.cores, chip.revision,
           CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, (unsigned)esp_psram_get_size(),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void app_main(void)
{
    usb_serial_jtag_driver_config_t usb = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
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
        } else {
            printf("@{\"error\":\"unknown command\",\"line\":\"%.40s\"}\n", line);
        }
        printf("@{\"cmd\":\"%.40s\",\"done\":true}\n", line);
    }
}
