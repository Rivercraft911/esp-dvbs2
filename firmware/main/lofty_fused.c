// Fused and scaled kernels share the same source and rounding rules.
#ifdef __XTENSA__
#define LOFTY_TEXT __attribute__((section(".lofty_fused"), noinline))
#endif
#define LOFTY_FUSED 1
#define lofty_build lofty_fused_build
#define lofty_prepare lofty_fused_prepare
#define lofty_prepare_pie lofty_fused_prepare_pie
#define lofty_render lofty_fused_render
#define lofty_render_pie lofty_fused_render_pie
#define lofty_render_pages lofty_fused_render_pages
#define lofty_render_pages_pie lofty_fused_render_pages_pie
#define lofty_samples lofty_fused_samples
#include "lofty.c"
