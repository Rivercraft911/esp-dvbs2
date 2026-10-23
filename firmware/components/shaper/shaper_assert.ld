/* ESP32-S3 instruction RAM aperture, TRM v1.8 table 4.3-1, p403. */
ASSERT(machine_axis_render_pie >= 0x40370000 && machine_axis_render_pie < 0x403e0000,
       "fractional vector renderer must be in IRAM")
ASSERT(machine_axis_samples >= 0x40370000 && machine_axis_samples < 0x403e0000,
       "fractional bank-cut wrapper must be in IRAM")
ASSERT(shaper_copy_pie >= 0x40370000 && shaper_copy_pie < 0x403e0000,
       "fractional cut copies must be in IRAM")
ASSERT(lofty_render_pages_pie >= 0x40370000 && lofty_render_pages_pie < 0x403e0000,
       "lofty_render_pages_pie must be in IRAM: its alignment class below is an IRAM layout")
ASSERT(lofty_render_pie >= 0x40370000 && lofty_render_pie < 0x403e0000,
       "lofty_render_pie must be in IRAM: its alignment class below is an IRAM layout")
ASSERT(shaper_short4_pie >= 0x40370000 && shaper_short4_pie < 0x403e0000,
       "shaper_short4_pie must be in IRAM: its alignment class below is an IRAM layout")
ASSERT(shaper_qpsk3_pie >= 0x40370000 && shaper_qpsk3_pie < 0x403e0000,
       "shaper_qpsk3_pie must be in IRAM: its alignment class below is an IRAM layout")
ASSERT(shaper_qpsk4_pie >= 0x40370000 && shaper_qpsk4_pie < 0x403e0000,
       "shaper_qpsk4_pie must be in IRAM: its alignment class below is an IRAM layout")
ASSERT(ring_copy_quads_pie >= 0x40370000 && ring_copy_quads_pie < 0x403e0000,
       "ring_copy_quads_pie must be in IRAM: its alignment class below is an IRAM layout")
ASSERT(shaper_fill4_pie >= 0x40370000 && shaper_fill4_pie < 0x403e0000,
       "shaper_fill4_pie must be in IRAM: its alignment class below is an IRAM layout")
ASSERT(shaper_fill8_pie >= 0x40370000 && shaper_fill8_pie < 0x403e0000,
       "shaper_fill8_pie must be in IRAM: its alignment class below is an IRAM layout")
ASSERT(shaper_fill40_pie >= 0x40370000 && shaper_fill40_pie < 0x403e0000,
       "shaper_fill40_pie must be in IRAM: its alignment class below is an IRAM layout")
ASSERT(shaper_unpack_codes_pie >= 0x40370000 && shaper_unpack_codes_pie < 0x403e0000,
       "shaper_unpack_codes_pie must be in IRAM: its alignment class below is an IRAM layout")
ASSERT(shaper_axes8_pie >= 0x40370000 && shaper_axes8_pie < 0x403e0000,
       "shaper_axes8_pie must be in IRAM: its alignment class below is an IRAM layout")
ASSERT((lofty_render_pages_pie & 31) == 4 && (lofty_render_pie & 31) == 20,
       "8PSK renderer loop fetch alignment must match the measured 7391f86 layout")
ASSERT((machine_axis_render_pie & 31) == 0 && (shaper_short4_pie & 31) == 24 &&
       (shaper_qpsk3_pie & 31) == 20 && (shaper_qpsk4_pie & 31) == 28 &&
       (ring_copy_quads_pie & 31) == 12 && (shaper_copy_pie & 31) == 4 &&
       (shaper_fill4_pie & 31) == 8 && (shaper_fill8_pie & 31) == 0 &&
       (shaper_fill40_pie & 31) == 24 && (shaper_unpack_codes_pie & 31) == 0,
       "fill kernel fetch alignment must match the measured 7391f86 layout")
ASSERT((shaper_axes8_pie & 31) == 8,
       "VCM axis loop fetch alignment must match modswap 154692d")

ASSERT((_lofty_text_start & 31) == 0 &&
       ((_lofty_text_end - _lofty_text_start + 31) & ~31) <= 4096,
       "lofty control code must fit one aligned instruction-cache way")
ASSERT(_lofty_text_start >= 0x42000000 && _lofty_text_end < 0x42800000,
       "lofty control code must have flash backing")
ASSERT((_lofty_text_start & 4095) == 0x720,
       "lofty cache lock indices must match the measured 7391f86 layout")
