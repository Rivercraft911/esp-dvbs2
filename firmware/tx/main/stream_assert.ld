/* ESP32-S3 instruction RAM aperture, TRM v1.8 table 4.3-1, p403. */
ASSERT(xTaskGenericNotify >= 0x40370000 && xTaskGenericNotify < 0x403e0000,
       "stream notification must be in IRAM")
ASSERT(shaper_axes8_pie >= _vcm_text_start && shaper_axes8_pie < _vcm_text_end,
       "VCM axis renderer must be in its separate IRAM region")
ASSERT(_vcm_text_start >= 0x40370000 && _vcm_text_end < 0x403e0000,
       "VCM hot code must use SRAM instruction fetches")
ASSERT(_vcm_text_end <= _iram_text_end && _iram_text_end <= _iram_end,
       "VCM IRAM must be executable and reserved from the DRAM heap")
ASSERT(shaper_store >= 0x3fc88000 && shaper_store + 65536 <= 0x3fd00000,
       "fractional tables, queue and scratch must be internal DRAM")
ASSERT((_stream_bss_start & 15) == 0 && _stream_bss_start >= 0x3fc88000 &&
       _stream_bss_end <= 0x3fcc0000,
       "stream state must be 16-byte aligned internal DRAM below the DAC banks")
