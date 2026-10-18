#pragma once
#include <stddef.h>
#ifdef __XTENSA__
#define VCM_TEXT __attribute__((section(".vcm"), noinline))
#define VCM_BSS __attribute__((section(".vcm_bss")))
#else
#define VCM_TEXT
#define VCM_BSS
#endif
int vcm_resident(int on);
size_t vcm_resident_bytes(void);
