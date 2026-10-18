// QEMU: install real dispatch-table observers on both CPUs, fire software
// interrupts, restore the original handlers, then fire them again. No radio.
#include <assert.h>
#include <stdio.h>
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_intr_alloc.h"
#include "esp_ipc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "xtensa_api.h"
#include "stream_jitter.h"

void radio_s3_profile_start(void);
void radio_s3_profile_stop(void);
void radio_s3_profile_bank_begin(void);
void radio_s3_profile_bank_end(uint32_t cycles, unsigned bank, uint64_t first);
static intr_handle_t handle[2];
static volatile unsigned hits[2];
static SemaphoreHandle_t done;

static void IRAM_ATTR hit(void *arg) { ++*(volatile unsigned *)arg; }
static void allocate(void *arg)
{
    (void)arg;
    unsigned c = esp_cpu_get_core_id();
    ESP_ERROR_CHECK(esp_intr_alloc(ETS_INTERNAL_SW0_INTR_SOURCE,ESP_INTR_FLAG_IRAM,hit,(void *)&hits[c],&handle[c]));
}
static void fire(void *arg)
{
    (void)arg;
    unsigned c = esp_cpu_get_core_id(), before = hits[c];
    xt_set_intset(1u<<esp_intr_get_intno(handle[c]));
    while (hits[c] == before) __asm__ volatile("nop");
    assert(hits[c] == before+1);
}
static void free_interrupt(void *arg)
{
    (void)arg;
    ESP_ERROR_CHECK(esp_intr_free(handle[esp_cpu_get_core_id()]));
}
static void worker(void *arg)
{
    (void)arg;
    ESP_ERROR_CHECK(esp_ipc_call_blocking(0,allocate,NULL));
    allocate(NULL);
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
        stream_jitter_enable(1);
        radio_s3_profile_start();
        ESP_ERROR_CHECK(esp_ipc_call_blocking(0,fire,NULL));
        radio_s3_profile_bank_begin();
        fire(NULL);
        radio_s3_profile_bank_end(12345,1,0); // synthetic bank length, not a timing result
        radio_s3_profile_stop();
        printf("@{\"test\":\"jitter_target\""); stream_jitter_print(); puts("}");
        ESP_ERROR_CHECK(esp_ipc_call_blocking(0,fire,NULL));
        fire(NULL);
    }
    assert(hits[0] == 4 && hits[1] == 4);
    ESP_ERROR_CHECK(esp_ipc_call_blocking(0,free_interrupt,NULL));
    free_interrupt(NULL);
    xSemaphoreGive(done);
    vTaskDelete(NULL);
}
void jitter_target_check(void)
{
    done = xSemaphoreCreateBinary();
    assert(done);
    assert(xTaskCreatePinnedToCore(worker,"jitter",4096,NULL,5,NULL,1) == pdPASS);
    assert(xSemaphoreTake(done,pdMS_TO_TICKS(5000)) == pdTRUE);
    vSemaphoreDelete(done);
}
