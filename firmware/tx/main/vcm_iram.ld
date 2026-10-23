/* A separate output section prevents Xtensa literal merging from moving the
 * qualified default .iram0.text. IDF's following _iram_end / DRAM reservation
 * include this region. Keep VCM on the SRAM instruction bus, as in modswap. */
SECTIONS
{
  .iram_vcm : ALIGN(4)
  {
    _vcm_text_start = ABSOLUTE(.);
    *(.vcm.literal .vcm)
    *(.vcm.axis.literal .vcm.axis)
    _vcm_text_end = ABSOLUTE(.);
    . += 16;
    . = ALIGN(256);
    _iram_text_end = ABSOLUTE(.);
  } > iram0_0_seg
}
INSERT AFTER .iram0.text;
