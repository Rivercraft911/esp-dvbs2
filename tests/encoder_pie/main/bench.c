// QEMU has no PSRAM. Exercise the actual command with all buffers internal.
#include "esp_heap_caps.h"
#undef MALLOC_CAP_SPIRAM
#define MALLOC_CAP_SPIRAM MALLOC_CAP_INTERNAL
#include <stdint.h>
uint32_t shaper_store[16384] __attribute__((aligned(16)));
#include "../../../firmware/main/fecbench.c"
