// QEMU has no PSRAM. Exercise the actual command with all buffers internal.
#include "esp_heap_caps.h"
#undef MALLOC_CAP_SPIRAM
#define MALLOC_CAP_SPIRAM MALLOC_CAP_INTERNAL
#include "../../../firmware/main/fecbench.c"
