/* ESP32-S3 instruction RAM aperture, TRM v1.8 table 4.3-1, p403. */
ASSERT(xTaskGenericNotify >= 0x40370000 && xTaskGenericNotify < 0x403e0000,
       "stream notification must be in IRAM")
ASSERT(machine_axis_render_pie >= 0x40370000 && machine_axis_render_pie < 0x403e0000,
       "fractional vector renderer must be in IRAM")
ASSERT(machine_axis_samples >= 0x40370000 && machine_axis_samples < 0x403e0000,
       "fractional bank-cut wrapper must be in IRAM")
ASSERT(shaper_copy_pie >= 0x40370000 && shaper_copy_pie < 0x403e0000,
       "fractional cut copies must be in IRAM")
ASSERT(shaper_axes8_pie >= _vcm_text_start && shaper_axes8_pie < _vcm_text_end,
       "VCM axis renderer must be in its separate IRAM region")
ASSERT((shaper_axes8_pie & 31) == 8,
       "VCM axis loop fetch alignment must match modswap 154692d")
ASSERT(_vcm_text_start >= 0x40370000 && _vcm_text_end < 0x403e0000,
       "VCM hot code must use SRAM instruction fetches")
ASSERT(_vcm_text_end <= _iram_text_end && _iram_text_end <= _iram_end,
       "VCM IRAM must be executable and reserved from the DRAM heap")
ASSERT(lofty_render_pages_pie == 0x4037a324 && lofty_render_pie == 0x4037a334,
       "default vector renderer placement must match main 7391f86")
ASSERT(shaper_store >= 0x3fc88000 && shaper_store + 65536 <= 0x3fd00000,
       "fractional tables, queue and scratch must be internal DRAM")

ASSERT((_lofty_text_start & 31) == 0 &&
       ((_lofty_text_end - _lofty_text_start + 31) & ~31) <= 4096,
       "lofty control code must fit one aligned instruction-cache way")
ASSERT(_lofty_text_start >= 0x42000000 && _lofty_text_end < 0x42800000,
       "lofty control code must have flash backing")

ASSERT((_lofty_fused_text_start & 31) == 0 &&
       ((_lofty_fused_text_end - _lofty_fused_text_start + 31) & ~31) <= 4096,
       "fused control code must fit one instruction-cache way")

ASSERT((_lofty_text_end - _lofty_text_start) == 1632,
       "default cache lock footprint must match main 6e7eed5")
ASSERT((_lofty_text_start & 4095) == 0x720,
       "default cache lock indices must match main 7391f86")
