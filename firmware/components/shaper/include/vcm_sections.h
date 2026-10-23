#pragma once
// Separate output sections keep VCM's code/literals and state from changing
// the fixed fill's instruction layout. vcm_iram.ld reserves executable SRAM.
#ifdef __XTENSA__
#define VCM_TEXT __attribute__((section(".vcm"), noinline))
#define VCM_BSS __attribute__((section(".vcm_bss")))
#else
#define VCM_TEXT
#define VCM_BSS
#endif
