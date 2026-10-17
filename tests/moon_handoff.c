// Peripheral-free handoff instruction trace. CCOUNT is deliberately unused.
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static TaskHandle_t helper;
static unsigned pending, finished;

__attribute__((noinline)) void handoff_begin(void) { __asm__ volatile("nop"); }
__attribute__((noinline)) void handoff_end(void) { __asm__ volatile("nop; nop"); }

static void receiver(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        if (__atomic_load_n(&pending,__ATOMIC_ACQUIRE))
            __atomic_store_n(&pending,0,__ATOMIC_RELEASE);
    }
}

static void sender(void *arg)
{
    (void)arg;
    for (unsigned i = 0; i < 32; ++i) {
        handoff_begin();
        __atomic_store_n(&pending,1,__ATOMIC_RELEASE);
        xTaskNotifyGive(helper);
        handoff_end();
        while (__atomic_load_n(&pending,__ATOMIC_ACQUIRE)) { }
        // Let the receiver block again. The trace interval excludes this delay.
        vTaskDelay(1);
    }
    __atomic_store_n(&finished,1,__ATOMIC_RELEASE);
    vTaskDelete(NULL);
}

void app_main(void)
{
    puts("QEMU-START");
    configASSERT(xTaskCreatePinnedToCore(receiver,"helper",2048,NULL,6,&helper,0) == pdPASS);
    configASSERT(xTaskCreatePinnedToCore(sender,"sender",2048,NULL,6,NULL,1) == pdPASS);
    while (!__atomic_load_n(&finished,__ATOMIC_ACQUIRE)) vTaskDelay(1);
    vTaskDelete(helper);
    puts("QEMU-END");
}
