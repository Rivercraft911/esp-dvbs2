/* ESP32-S3 instruction RAM aperture, TRM v1.8 table 4.3-1, p403. */
ASSERT(xTaskGenericNotify >= 0x40370000 && xTaskGenericNotify < 0x403e0000,
       "stream notification must be in IRAM")
