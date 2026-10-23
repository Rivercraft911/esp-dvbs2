/* ESP32-S3 PIE versions of kernels.c (same results). The coprocessor is enabled lazily by
 * FreeRTOS on first use; callers may run with interrupts enabled. Clobbers q0..q7, ACCX, SAR. */
.section .iram1, "ax", @progbits

/* void rx_unpack(const uint32_t *words, size_t blocks, int8_t *i, int8_t *q)
 * a2 words, a3 blocks, a4 i, a5 q. I = word bits 19:12, Q = bits 9:2. */
.align 4
.global rx_unpack
.type rx_unpack, @function
rx_unpack:
    entry a1, 32
    movi a6, 12
    movi a7, 2
    loopnez a3, 1f
    ee.vld.128.ip q0, a2, 16
    ee.vld.128.ip q1, a2, 16
    ee.vld.128.ip q2, a2, 16
    ee.vld.128.ip q3, a2, 16
    wsr a6, sar
    ee.vsr.32 q4, q0
    ee.vsr.32 q5, q1
    ee.vsr.32 q6, q2
    ee.vsr.32 q7, q3
    ee.vunzip.16 q4, q5
    ee.vunzip.16 q6, q7
    ee.vunzip.8 q4, q6
    ee.vst.128.ip q4, a4, 16
    wsr a7, sar
    ee.vsr.32 q0, q0
    ee.vsr.32 q1, q1
    ee.vsr.32 q2, q2
    ee.vsr.32 q3, q3
    ee.vunzip.16 q0, q1
    ee.vunzip.16 q2, q3
    ee.vunzip.8 q0, q2
    ee.vst.128.ip q0, a5, 16
1:
    retw.n
.size rx_unpack, .-rx_unpack

/* void rx_mf(const int8_t *i, const int8_t *q, const rx_mf_step_t *steps, size_t n,
 *            unsigned vectors, int32_t *out)
 * a2 i, a3 q, a4 steps {base, coef}, a5 n, a6 vectors, a7 out (I, Q pairs).
 * Seven vectors: the set stays in q1..q7 for both axes and data loads ride on the
 * multiplies. Other counts take the plain loop. */
.align 4
.global rx_mf
.type rx_mf, @function
rx_mf:
    entry a1, 32
    beqz a5, 3f
    movi a8, 7
    bne a6, a8, 4f
    loopnez a5, 3f
    l32i a8, a4, 0              /* base */
    l32i a9, a4, 4              /* coefficient set */
    addi a4, a4, 8
    add a10, a2, a8
    add a11, a3, a8
    ee.zero.accx
    ee.vld.128.ip q1, a9, 16
    ee.vld.128.ip q0, a10, 16
    ee.vld.128.ip q2, a9, 16
    ee.vmulas.s8.accx.ld.ip q0, a10, 16, q0, q1
    ee.vld.128.ip q3, a9, 16
    ee.vmulas.s8.accx.ld.ip q0, a10, 16, q0, q2
    ee.vld.128.ip q4, a9, 16
    ee.vmulas.s8.accx.ld.ip q0, a10, 16, q0, q3
    ee.vld.128.ip q5, a9, 16
    ee.vmulas.s8.accx.ld.ip q0, a10, 16, q0, q4
    ee.vld.128.ip q6, a9, 16
    ee.vmulas.s8.accx.ld.ip q0, a10, 16, q0, q5
    ee.vld.128.ip q7, a9, 16
    ee.vmulas.s8.accx.ld.ip q0, a10, 16, q0, q6
    ee.vmulas.s8.accx.ld.ip q0, a11, 16, q0, q7
    rur.accx_0 a13
    s32i a13, a7, 0
    ee.zero.accx
    ee.vmulas.s8.accx.ld.ip q0, a11, 16, q0, q1
    ee.vmulas.s8.accx.ld.ip q0, a11, 16, q0, q2
    ee.vmulas.s8.accx.ld.ip q0, a11, 16, q0, q3
    ee.vmulas.s8.accx.ld.ip q0, a11, 16, q0, q4
    ee.vmulas.s8.accx.ld.ip q0, a11, 16, q0, q5
    ee.vmulas.s8.accx.ld.ip q0, a11, 16, q0, q6
    ee.vmulas.s8.accx q0, q7
    rur.accx_0 a13
    s32i a13, a7, 4
    addi a7, a7, 8
3:
    retw.n
4:
    l32i a8, a4, 0
    l32i a9, a4, 4
    addi a4, a4, 8
    add a10, a2, a8
    add a11, a3, a8
    ee.zero.accx
    mov a12, a9
    loopnez a6, 1f
    ee.vld.128.ip q0, a10, 16
    ee.vld.128.ip q1, a12, 16
    ee.vmulas.s8.accx q0, q1
1:
    rur.accx_0 a13
    s32i a13, a7, 0
    ee.zero.accx
    loopnez a6, 1f
    ee.vld.128.ip q0, a11, 16
    ee.vld.128.ip q1, a9, 16
    ee.vmulas.s8.accx q0, q1
1:
    rur.accx_0 a13
    s32i a13, a7, 4
    addi a7, a7, 8
    addi a5, a5, -1
    bnez a5, 4b
    retw.n
.size rx_mf, .-rx_mf
