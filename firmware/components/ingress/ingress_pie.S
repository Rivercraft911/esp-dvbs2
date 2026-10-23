/* Payload copy out of the ingress slots on ESP32-S3 PIE (ingress.c, ingress_read).
 * void ingress_copy_pie(uint8_t *dst, const uint8_t *src, unsigned blocks)
 *   dst 16-byte aligned, src any alignment, blocks >= 1: copies 16 * blocks bytes.
 *   Reads only the aligned 16-byte blocks that hold src[0 .. 16 * blocks): with src unaligned,
 *   EE.LD.128.USAR.IP loads the first block and sets SAR_BYTE = src & 15, and EE.SRC.Q joins each
 *   pair of neighbouring blocks (ESP32-S3 TRM v1.8 1.8.17, 1.8.49, as in ts_pie.S); with src
 *   aligned, plain loads, since the last USAR block would hold no source byte. */

    .section .iram1.ingress_copy, "ax"
    .align 4
    .global ingress_copy_pie
    .type ingress_copy_pie, @function
ingress_copy_pie:
    entry a1, 32
    extui a5, a3, 0, 4
    beqz a5, 2f
    addi a4, a4, -1                     /* the loop joins blocks - 1 pairs, the end the last */
    ee.ld.128.usar.ip q0, a3, 16        /* the block holding src[0] */
    ee.vld.128.ip q1, a3, 16            /* the next block holds the rest of these 16 bytes */
    loopnez a4, 1f
    ee.src.q.qup q2, q0, q1             /* q2 = 16 source bytes, q0 = q1 */
    ee.vld.128.ip q1, a3, 16            /* one block ahead, loaded only while it is needed */
    ee.vst.128.ip q2, a2, 16
1:
    ee.src.q.qup q2, q0, q1
    ee.vst.128.ip q2, a2, 16
    retw
2:
    loopnez a4, 3f
    ee.vld.128.ip q0, a3, 16
    ee.vst.128.ip q0, a2, 16
3:
    retw
    .size ingress_copy_pie, . - ingress_copy_pie
