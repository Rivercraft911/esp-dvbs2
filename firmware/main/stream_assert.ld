/* ESP32-S3 instruction RAM aperture, TRM v1.8 table 4.3-1, p403. */
ASSERT(xTaskGenericNotify >= 0x40370000 && xTaskGenericNotify < 0x403e0000,
       "stream notification must be in IRAM")
ASSERT(machine_axis_render_pie >= 0x40370000 && machine_axis_render_pie < 0x403e0000,
       "fractional vector renderer must be in IRAM")
ASSERT(machine_axis_samples >= 0x40370000 && machine_axis_samples < 0x403e0000,
       "fractional bank-cut wrapper must be in IRAM")
ASSERT(shaper_copy_pie >= 0x40370000 && shaper_copy_pie < 0x403e0000,
       "fractional cut copies must be in IRAM")
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
