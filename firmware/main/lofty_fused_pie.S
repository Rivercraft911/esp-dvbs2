// Fused and scaled kernels share the same source and rounding rules.
#define LOFTY_FUSED 1
#define lofty_prepare_pie lofty_fused_prepare_pie
#define lofty_render_pie lofty_fused_render_pie
#define lofty_render_pages_pie lofty_fused_render_pages_pie
#include "lofty_pie.S"
