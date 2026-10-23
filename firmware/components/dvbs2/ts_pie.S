/* TS CRC-8 on ESP32-S3 PIE, by columns instead of tables (fast.c, dvbs2_fast_bb_ts).
 * CRC-8 is linear with a zero start, so the CRC of a packet's 187 data bytes is the XOR of
 * one column byte per set data bit: col[i][k] = CRC of a packet holding only bit k of byte i.
 * This kernel takes the packet sixteen bytes at a time, twelve times; the five bytes after the
 * packet in the last chunk meet zero columns. Per byte position and bit, EE.VCMP.LT.S8 turns the current
 * top bit into a 0x00/0xff mask, which selects that bit's column. EE.VSL.32 then shifts the
 * next bit up: within eight shifts no bit crosses into a neighbour's top bit.
 * Same semantics notes as pie.S (ESP32-S3 TRM v1.8 chapter 1).
 *
 * uint32_t dvbs2_pie_crc8_packet(const uint8_t *p, const uint8_t *cols)
 *   p: the packet's 187 data bytes, any alignment; reads the 14 aligned blocks from
 *   align_down(p, 16), through align_down(p, 16) + 223 (fast.c checks they are in the buffer).
 *   cols: 16-byte aligned, 12 chunks x 8 bits (top bit first) x 16 positions, then 16 spare
 *   bytes that the last iteration loads and ignores. */

    .section .iram1, "ax"
    .align 4
    .global dvbs2_pie_crc8_packet
    .type dvbs2_pie_crc8_packet, @function
dvbs2_pie_crc8_packet:
    entry a1, 32
    movi a4, 1
    wsr.sar a4                          /* EE.VSL.32 shifts by SAR */
    ee.zero.q q7                        /* compare against zero */
    ee.zero.q q6                        /* XOR of the selected columns */
    ee.ld.128.usar.ip q0, a2, 16        /* SAR_BYTE = p & 15 */
    ee.vld.128.ip q1, a2, 16
    ee.src.q.qup q2, q0, q1             /* q2 = bytes 0..15, q0 = q1 */
    ee.vld.128.ip q4, a3, 16            /* chunk 0, top bit column */
    movi a5, 12
    /* Each result is used two or more instructions after it is made. The last chunk loads
     * one block and one column past the end; cols carries 16 spare bytes for that. */
    loopnez a5, 1f
    .rept 7
    ee.vcmp.lt.s8 q3, q2, q7            /* 0xff where the top bit is set */
    ee.vsl.32 q2, q2                    /* next bit to the top */
    ee.andq q3, q3, q4
    ee.vld.128.ip q4, a3, 16            /* next bit's column */
    ee.xorq q6, q6, q3
    .endr
    ee.vcmp.lt.s8 q3, q2, q7            /* bit 0 */
    ee.vld.128.ip q1, a2, 16            /* next data block */
    ee.andq q3, q3, q4
    ee.vld.128.ip q4, a3, 16            /* next chunk's top-bit column */
    ee.src.q.qup q2, q0, q1             /* next 16 data bytes, q0 = q1 */
    ee.xorq q6, q6, q3
1:
    ee.movi.32.a q6, a2, 0
    ee.movi.32.a q6, a4, 1
    xor a2, a2, a4
    ee.movi.32.a q6, a4, 2
    xor a2, a2, a4
    ee.movi.32.a q6, a4, 3
    xor a2, a2, a4
    srli a4, a2, 16
    xor a2, a2, a4
    srli a4, a2, 8
    xor a2, a2, a4
    extui a2, a2, 0, 8
    retw
    .size dvbs2_pie_crc8_packet, . - dvbs2_pie_crc8_packet
