#include "vcm_cache.h"
#if defined(__XTENSA__) || defined(VCM_CACHE_HOST)
#include <stdint.h>
#ifndef VCM_CACHE_HOST
#include "esp_cpu.h"
#include "sdkconfig.h"
_Static_assert(CONFIG_ESP32S3_INSTRUCTION_CACHE_SIZE == 16384 &&
               CONFIG_ESP32S3_INSTRUCTION_CACHE_LINE_SIZE == 32,
               "VCM residency requires the qualified 16 KiB / 32-byte cache");
#include "esp32s3/rom/cache.h"
extern char _vcm_text_start[], _vcm_text_end[];
#endif
static int resident VCM_BSS;
#endif

int vcm_resident(int on)
{
#if defined(__XTENSA__) || defined(VCM_CACHE_HOST)
    if (!!on == resident) return 0;
    const uint32_t first=(uintptr_t)_vcm_text_start;
    const uint32_t bytes=((uintptr_t)_vcm_text_end-first+31)&~31u;
    if (!on) {
        int result=Cache_Unlock_Addr(first,bytes);
        if (!result) resident=0;
        return result;
    }
    const uint32_t before=esp_cpu_get_cycle_count();
    const uint32_t autoload=Cache_Start_ICache_Preload(first,bytes,0);
    while (!Cache_ICache_Preload_Done()) {
        if (esp_cpu_get_cycle_count()-before>240000) {
            Cache_End_ICache_Preload(autoload);
            return -1;
        }
    }
    Cache_End_ICache_Preload(autoload);
    int result=Cache_Lock_Addr(first,bytes);
    if (!result) resident=1;
    return result;
#else
    (void)on;
    return 0;
#endif
}

size_t vcm_resident_bytes(void)
{
#if defined(__XTENSA__) || defined(VCM_CACHE_HOST)
    return resident ? ((uintptr_t)_vcm_text_end-(uintptr_t)_vcm_text_start+31)&~(size_t)31 : 0;
#else
    return 0;
#endif
}
