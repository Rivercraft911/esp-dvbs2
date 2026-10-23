/* ESP32-S3 PIE versions of demod.c's heavy kernels (demod_pie.h); same bytes as the C.
 * Semantics relied on, checked on the board: EE.VMUL.S16 keeps the low 16 bits of
 * floor(x y / 2^SAR); VADDS/VSUBS.S16 saturate; VCMP sets lanes to all ones or zero;
 * VLDBC.16 broadcasts one 16-bit word; VZIP.16 a, b leaves the low halves interleaved in a
 * and the high halves in b; VUNZIP.8 a, b leaves the even bytes in a; VMULAS.U16.ACCX adds
 * the unsigned products to ACCX; SRS.ACCX shifts ACCX right in place and returns it.
 * Clobbers q0..q7, SAR and ACCX. Windowed ABI: a2..a7. */
    .section .iram1, "ax"

/* demod_run_t offsets */
    .set CO, 0
    .set SO, 16
    .set LR, 32
    .set LI, 48
    .set DR, 64
    .set DI, 80
    .set SEL, 96
    .set GAIN, 128
    .set WEAK, 192
    .set TABLE, 256

/* demod_out_t offsets: hard[3], weak[3] */
    .set HARD0, 0
    .set WEAK0, 12

    .set STRIDE, 4352                        /* DEMOD_STRIDE: rq - ri */
    .set MF5_SHIFT, 2                        /* RX_MF5_SHIFT */

/* void demod_halves_pie(const int8_t *xi, const int8_t *xq, unsigned n, int16_t *hi,
 *                       int16_t *hq, const int8_t *w)
 * Two candidates per vector: the products with w's two halves, summed in ACCX. */
    .align 4
    .global demod_halves_pie
    .type demod_halves_pie, @function
demod_halves_pie:
    entry a1, 32
    srli a8, a4, 1
    beqz a8, 2f
    ee.vld.128.ip q6, a7, 16
    ee.vld.128.ip q7, a7, 0
    loopnez a8, 1f
    ee.vld.128.ip q0, a2, 16
    ee.vld.128.ip q1, a3, 16
    ee.zero.accx
    ee.vmulas.s8.accx q0, q6
    rur.accx_0 a9
    ee.zero.accx
    ee.vmulas.s8.accx q0, q7
    rur.accx_0 a10
    ee.zero.accx
    ee.vmulas.s8.accx q1, q6
    rur.accx_0 a11
    ee.zero.accx
    ee.vmulas.s8.accx q1, q7
    rur.accx_0 a12
    s16i a9, a5, 0
    s16i a10, a5, 2
    s16i a11, a6, 0
    s16i a12, a6, 2
    addi a5, a5, 4
    addi a6, a6, 4
1:
2:
    retw
    .size demod_halves_pie, . - demod_halves_pie

/* void demod_mf_pie(demod_mf_t *m, unsigned n, int16_t *oi, int16_t *oq)
 * a6 ri, a8 sets, a10/a11 time, a12 step, a7/a9 sums. Per output: tq = t >> 22 (quarter
 * samples), s0 = tq / 4 - 47, the window at ri + (s0 & 4080) and + 4352 for Q, the set
 * ((s0 & 15) * 4 + (tq & 3)) * 112; seven vectors per axis as in rx_mf. */
    .align 4
    .global demod_mf_pie
    .type demod_mf_pie, @function
demod_mf_pie:
    entry a1, 32
    beqz a3, 2f
    l32i a6, a2, 0
    l32i a8, a2, 8
    l32i a10, a2, 12
    l32i a11, a2, 16
    l32i a12, a2, 20
    l32i a7, a2, 24
    l32i a9, a2, 28
    ssai 22
    loopnez a3, 1f
    src a3, a11, a10                         /* tq */
    srli a13, a3, 2
    addi a13, a13, -47                       /* s0 */
    extui a14, a13, 4, 8
    slli a14, a14, 4
    add a14, a14, a6                         /* I window */
    extui a15, a13, 0, 4
    extui a13, a3, 0, 2
    addx4 a15, a15, a13
    subx8 a15, a15, a15
    slli a15, a15, 4
    add a15, a15, a8                         /* coefficient set */
    addmi a13, a14, 4352                     /* Q window */
    add a3, a10, a12
    bgeu a3, a10, 3f
    addi a11, a11, 1
3:
    mov a10, a3
    ee.zero.accx
    ee.vld.128.ip q1, a15, 16
    ee.vld.128.ip q0, a14, 16
    ee.vld.128.ip q2, a15, 16
    ee.vmulas.s8.accx.ld.ip q0, a14, 16, q0, q1
    ee.vld.128.ip q3, a15, 16
    ee.vmulas.s8.accx.ld.ip q0, a14, 16, q0, q2
    ee.vld.128.ip q4, a15, 16
    ee.vmulas.s8.accx.ld.ip q0, a14, 16, q0, q3
    ee.vld.128.ip q5, a15, 16
    ee.vmulas.s8.accx.ld.ip q0, a14, 16, q0, q4
    ee.vld.128.ip q6, a15, 16
    ee.vmulas.s8.accx.ld.ip q0, a14, 16, q0, q5
    ee.vld.128.ip q7, a15, 16
    ee.vmulas.s8.accx.ld.ip q0, a14, 16, q0, q6
    ee.vmulas.s8.accx.ld.ip q0, a13, 16, q0, q7
    rur.accx_0 a3
    srai a3, a3, 4
    s16i a3, a4, 0
    add a7, a7, a3
    ee.zero.accx
    ee.vmulas.s8.accx.ld.ip q0, a13, 16, q0, q1
    ee.vmulas.s8.accx.ld.ip q0, a13, 16, q0, q2
    ee.vmulas.s8.accx.ld.ip q0, a13, 16, q0, q3
    ee.vmulas.s8.accx.ld.ip q0, a13, 16, q0, q4
    ee.vmulas.s8.accx.ld.ip q0, a13, 16, q0, q5
    ee.vmulas.s8.accx.ld.ip q0, a13, 16, q0, q6
    ee.vmulas.s8.accx q0, q7
    rur.accx_0 a3
    srai a3, a3, 4
    s16i a3, a5, 0
    add a9, a9, a3
    addi a4, a4, 2
    addi a5, a5, 2
1:
    s32i a10, a2, 12
    s32i a11, a2, 16
    s32i a7, a2, 24
    s32i a9, a2, 28
2:
    retw
    .size demod_mf_pie, . - demod_mf_pie

/* void demod_turn_pie(int16_t *yi, int16_t *yq, unsigned groups, uint32_t phase,
 *                     uint32_t step8, const demod_run_t *k)
 * q0 co, q1 so. Per group: x = y - dc; C = cg co - sg so, S = sg co + cg so (cg, sg broadcast
 * from the table), then I = xi C + xq S - lr, Q = xq C - xi S - li, in place. */
    .align 4
    .global demod_turn_pie
    .type demod_turn_pie, @function
demod_turn_pie:
    entry a1, 32
    beqz a4, 2f
    movi a8, 14
    wsr.sar a8
    l32i a9, a7, TABLE
    addi a10, a7, LR
    addi a15, a7, LI
    addi a8, a7, DR
    mov a11, a2
    mov a12, a3
    ee.vld.128.ip q0, a7, 16                 /* co */
    ee.vld.128.ip q1, a7, 0                  /* so */
    loopnez a4, 1f
    srli a13, a5, 22
    addx2 a14, a13, a9                       /* &sin[a] */
    addi a13, a13, 256
    extui a13, a13, 0, 10
    addx2 a13, a13, a9                       /* &sin[(a + 256) & 1023] = &cos[a] */
    ee.vldbc.16 q4, a13                      /* cg */
    ee.vldbc.16 q5, a14                      /* sg */
    ee.vld.128.ip q2, a2, 16                 /* yi */
    ee.vld.128.ip q3, a3, 16                 /* yq */
    ee.vld.128.ip q6, a8, 16                 /* dr */
    ee.vld.128.ip q7, a8, -16                /* di */
    add a5, a5, a6
    ee.vsubs.s16 q2, q2, q6
    ee.vsubs.s16 q3, q3, q7
    ee.vmul.s16 q6, q4, q0                   /* cg co */
    ee.vmul.s16 q7, q5, q1                   /* sg so */
    ee.vsubs.s16 q6, q6, q7                  /* C */
    ee.vmul.s16 q7, q5, q0                   /* sg co */
    ee.vmul.s16 q5, q4, q1                   /* cg so */
    ee.vadds.s16 q7, q7, q5                  /* S */
    ee.vmul.s16 q4, q2, q6                   /* yi C */
    ee.vmul.s16 q5, q3, q7                   /* yq S */
    ee.vadds.s16 q4, q4, q5                  /* I */
    ee.vmul.s16 q5, q3, q6                   /* yq C */
    ee.vld.128.ip q3, a15, 0                 /* li */
    ee.vmul.s16 q6, q2, q7                   /* yi S */
    ee.vld.128.ip q2, a10, 0                 /* lr */
    ee.vsubs.s16 q5, q5, q6                  /* Q */
    ee.vsubs.s16 q4, q4, q2
    ee.vsubs.s16 q5, q5, q3
    ee.vst.128.ip q4, a11, 16
    ee.vst.128.ip q5, a12, 16
1:
2:
    retw
    .size demod_turn_pie, . - demod_turn_pie

/* uint64_t demod_signs_pie(const int16_t *hi, const int16_t *hq, int16_t *a, int32_t *sums,
 *                          int16_t *scratch)
 * Per axis, 8 candidates a pass: a = h + h shifted one lane (EE.SRC.Q at SAR_BYTE 2), a two
 * candidates back = the previous a and this one at SAR_BYTE 12, both to scratch (a_I, a_Q,
 * back_I, back_Q, 128 bytes each), h summed for the DC. Then each candidate's turn
 * aq back_i - ai back_q exactly in QACC, its sign by VCMP, and 8 signs a byte through ACCX with
 * weights 128 .. 1 (minus the byte, as the lanes are -1). */
    .macro pairs h, ap, ao, bo, off
    ee.vld.128.ip q1, \h, 16                 /* h[0..7] */
    ee.vld.128.ip q4, \ap, 0                 /* a[-2], a[-1] in lanes 6, 7 */
    ee.zero.q q6
    movi a12, 8
    loopnez a12, .Lpairs\@
    ee.vld.128.ip q2, \h, 16                 /* h[8b + 8 ..] */
    wur.sar_byte a7
    ee.src.q q3, q1, q2                      /* h[8b + 1 ..] */
    ee.vadds.s16 q5, q1, q3                  /* a[8b ..] */
    ee.vadds.s16 q6, q6, q3
    wur.sar_byte a8
    ee.src.q q4, q4, q5                      /* a[8b - 2 ..] */
    ee.vst.128.ip q5, \ao, 16
    ee.vst.128.ip q4, \bo, 16
    ee.orq q4, q5, q5
    ee.orq q1, q2, q2
.Lpairs\@:
    ee.vst.128.ip q4, \ap, 16                /* a[56..63] for the next block */
    ee.zero.accx
    ee.vmulas.s16.accx q6, q0
    rur.accx_0 a12
    l32i a13, a5, \off
    add a13, a13, a12
    s32i a13, a5, \off
    .endm

    .align 4
    .global demod_signs_pie
    .type demod_signs_pie, @function
demod_signs_pie:
    entry a1, 32
    movi a7, 2
    movi a8, 12
    movi a9, 0x00010001
    ee.movi.32.q q0, a9, 0
    ee.movi.32.q q0, a9, 1
    ee.movi.32.q q0, a9, 2
    ee.movi.32.q q0, a9, 3                   /* ones */
    mov a10, a6
    addmi a11, a6, 256
    pairs a2, a4, a10, a11, 0
    pairs a3, a4, a10, a11, 4                /* a10, a11 continue at the Q halves */
    movi a9, 0x00400080
    ee.movi.32.q q0, a9, 0
    movi a9, 0x00100020
    ee.movi.32.q q0, a9, 1
    movi a9, 0x00040008
    ee.movi.32.q q0, a9, 2
    movi a9, 0x00010002
    ee.movi.32.q q0, a9, 3                   /* lane l: 128 >> l */
    ee.zero.q q7
    mov a10, a6
    addi a11, a6, 64
    addi a11, a11, 64
    addmi a12, a6, 256
    addi a13, a12, 64
    addi a13, a13, 64
    movi a14, 0
    movi a15, 0
    movi a9, 0
    ssai 24
    movi a8, 8
    loopnez a8, 1f
    ee.vld.128.ip q1, a10, 16                /* ai */
    ee.vld.128.ip q2, a11, 16                /* aq */
    ee.vld.128.ip q3, a12, 16                /* back i */
    ee.vld.128.ip q4, a13, 16                /* back q */
    ee.zero.qacc
    ee.vmulas.s16.qacc q2, q3
    ee.vsubs.s16 q5, q7, q1
    ee.vmulas.s16.qacc q5, q4
    ee.srcmb.s16.qacc q6, a9, 0              /* the turns, saturated: the sign is exact */
    ee.vcmp.lt.s16 q6, q6, q7
    ee.zero.accx
    ee.vmulas.s16.accx q6, q0
    rur.accx_0 a7
    src a14, a14, a15
    slli a15, a15, 8
    sub a15, a15, a7
1:
    mov a2, a15
    mov a3, a14
    retw
    .size demod_signs_pie, . - demod_signs_pie

/* void demod_sum_pie(const int16_t *oi, const int16_t *oq, unsigned n, int32_t *sum)
 * Eight lanes a vector against ones in ACCX, per axis. */
    .align 4
    .global demod_sum_pie
    .type demod_sum_pie, @function
demod_sum_pie:
    entry a1, 32
    movi a6, 0x00010001
    ee.movi.32.q q7, a6, 0
    ee.movi.32.q q7, a6, 1
    ee.movi.32.q q7, a6, 2
    ee.movi.32.q q7, a6, 3                   /* ones */
    srli a4, a4, 3
    ee.zero.accx
    mov a6, a2
    loopnez a4, 1f
    ee.vld.128.ip q0, a6, 16
    ee.vmulas.s16.accx q0, q7
1:
    rur.accx_0 a7
    l32i a8, a5, 0
    add a8, a8, a7
    s32i a8, a5, 0
    ee.zero.accx
    mov a6, a3
    loopnez a4, 2f
    ee.vld.128.ip q0, a6, 16
    ee.vmulas.s16.accx q0, q7
2:
    rur.accx_0 a7
    l32i a8, a5, 4
    add a8, a8, a7
    s32i a8, a5, 4
    retw
    .size demod_sum_pie, . - demod_sum_pie

/* void demod_mf5_pie(const int8_t *x, const int8_t *sets, unsigned groups, int16_t *oi,
 *                    int16_t *oq)
 * a2 I window, a7 Q window, a11 the coefficient sets, a10 the shift. A group's five symbols
 * share its two I and two Q vectors (q4..q7); each symbol's set streams through q0, q1, the
 * next one loading on the Q products. Set 5 repeats set 0, so the stream rewinds 160 bytes a
 * group. */
    .align 4
    .global demod_mf5_pie
    .type demod_mf5_pie, @function
demod_mf5_pie:
    entry a1, 32
    beqz a4, 2f
    movi a10, MF5_SHIFT
    addmi a7, a2, STRIDE
    mov a11, a3
    ee.vld.128.ip q0, a11, 16
    ee.vld.128.ip q1, a11, 16
    loopnez a4, 1f
    ee.vld.128.ip q4, a2, 16                 /* I */
    ee.vld.128.ip q5, a2, 0                  /* a2 now at the next group's window */
    ee.vld.128.ip q6, a7, 16                 /* Q */
    ee.vld.128.ip q7, a7, 0
    .irp j, 0, 2, 4, 6, 8
    ee.zero.accx
    ee.vmulas.s8.accx q4, q0
    ee.vmulas.s8.accx q5, q1
    ee.srs.accx a8, a10, 0
    ee.zero.accx
    ee.vmulas.s8.accx.ld.ip q0, a11, 16, q6, q0
    ee.vmulas.s8.accx.ld.ip q1, a11, 16, q7, q1
    s16i a8, a5, \j
    ee.srs.accx a9, a10, 0
    s16i a9, a6, \j
    .endr
    addi a11, a11, -160
    addi a5, a5, 10
    addi a6, a6, 10
1:
2:
    retw
    .size demod_mf5_pie, . - demod_mf5_pie

/* Descramble one group of 8 (a2 yi, a3 yq, a5 scr; a10 walks the constants from sel, q4
 * zero): leaves x in q6, y in q7, a10 at GAIN. */
    .macro descramble8
    ee.vld.128.ip q0, a2, 16                 /* yi */
    ee.vld.128.ip q1, a3, 16                 /* yq */
    ee.vldbc.16.ip q2, a5, 2                 /* the 8 codes */
    ee.vld.128.ip q3, a10, 16                /* sel even */
    ee.vld.128.ip q5, a10, 16                /* sel high */
    ee.xorq q7, q0, q1
    ee.andq q3, q2, q3
    ee.andq q5, q2, q5
    ee.vcmp.eq.s16 q3, q3, q4                /* even: R even */
    ee.vcmp.eq.s16 q5, q5, q4                /* low: R < 2 */
    ee.andq q2, q7, q3                       /* e */
    ee.xorq q6, q1, q2                       /* x = R odd ? r : i ... */
    ee.xorq q7, q0, q2                       /* y */
    ee.notq q0, q5                           /* negate x where R >= 2 */
    ee.xorq q1, q3, q5                       /* negate y where R is 1 or 2 */
    ee.xorq q6, q6, q0
    ee.vsubs.s16 q6, q6, q0
    ee.xorq q7, q7, q1
    ee.vsubs.s16 q7, q7, q1
    .endm

/* Hard and weak bits of soft bits s (8 lanes in \sa, or 16 in \sa, \sb interleaved), q5
 * holding 16, a14 at the weights: hard bits in a15 bits 15..8 and weak in 7..0 (one vector),
 * or hard in a13 and weak in a15 (two). */
    .macro bits8 sa
    ee.vcmp.lt.s16 q1, \sa, q4               /* hard: s < 0 */
    ee.vsubs.s16 q2, q4, \sa
    ee.vmax.s16 q2, q2, \sa
    ee.vcmp.lt.s16 q2, q2, q5                /* weak: |s| < 16 */
    ee.vld.128.ip q3, a14, 16                /* weights 15..8 */
    ee.andq q1, q1, q3
    ee.vld.128.ip q3, a14, 16                /* weights 7..0 */
    ee.andq q2, q2, q3
    ee.vld.128.ip q3, a14, -32               /* ones */
    ee.zero.accx
    ee.vmulas.u16.accx q1, q3
    ee.vmulas.u16.accx q2, q3
    rur.accx_0 a15
    .endm

/* void demod_qpsk_pie(const int16_t *yi, const int16_t *yq, unsigned n, const uint8_t *scr,
 *                     const demod_run_t *k, const demod_out_t *o)
 * Per group: descramble, s = mul(x or y, gain), interleaved x0 y0 x1 y1 .., then 16 hard
 * and 16 weak bits through ACCX. */
    .macro qpsk_group
    addi a10, a6, SEL
    descramble8
    ee.vld.128.ip q2, a10, 0                 /* gain */
    ee.vmul.s16 q6, q6, q2
    ee.vmul.s16 q7, q7, q2
    ee.vzip.16 q6, q7                        /* x0 y0 x1 y1 .. | x4 y4 .. */
    mov a14, a7                              /* the constants from WEAK */
    ee.vld.128.ip q5, a14, 16                /* 16 */
    ee.vcmp.lt.s16 q0, q6, q4                /* hard, first 8 bits */
    ee.vcmp.lt.s16 q1, q7, q4                /* hard, last 8 */
    ee.vsubs.s16 q2, q4, q6
    ee.vmax.s16 q2, q2, q6
    ee.vcmp.lt.s16 q2, q2, q5                /* weak, first 8 */
    ee.vsubs.s16 q3, q4, q7
    ee.vmax.s16 q3, q3, q7
    ee.vcmp.lt.s16 q3, q3, q5                /* weak, last 8 */
    ee.vld.128.ip q5, a14, 16                /* weights 15..8 */
    ee.andq q0, q0, q5
    ee.andq q2, q2, q5
    ee.vld.128.ip q5, a14, 16                /* weights 7..0 */
    ee.andq q1, q1, q5
    ee.andq q3, q3, q5
    ee.vld.128.ip q5, a14, 0                 /* ones */
    ee.zero.accx
    ee.vmulas.u16.accx q0, q5
    ee.vmulas.u16.accx q1, q5
    rur.accx_0 a13                           /* hard */
    ee.zero.accx
    ee.vmulas.u16.accx q2, q5
    ee.vmulas.u16.accx q3, q5
    rur.accx_0 a15                           /* weak */
    .endm

    .align 4
    .global demod_qpsk_pie
    .type demod_qpsk_pie, @function
demod_qpsk_pie:
    entry a1, 32
    l32i a8, a7, HARD0
    l32i a9, a7, WEAK0
    movi a7, WEAK
    add a7, a6, a7
    movi a10, 14
    wsr.sar a10
    ee.zero.q q4
    srli a11, a4, 3
    loopnez a11, 1f
    qpsk_group
    srli a14, a13, 8
    s8i a14, a8, 0
    s8i a13, a8, 1
    srli a14, a15, 8
    s8i a14, a9, 0
    s8i a15, a9, 1
    addi a8, a8, 2
    addi a9, a9, 2
1:
    bbci a4, 2, 2f                           /* 4 symbols left */
    qpsk_group
    srli a14, a13, 8
    s8i a14, a8, 0
    srli a14, a15, 8
    s8i a14, a9, 0
2:
    retw
    .size demod_qpsk_pie, . - demod_qpsk_pie

/* void demod_8psk_pie(const int16_t *yi, const int16_t *yq, unsigned n, const uint8_t *scr,
 *                     const demod_run_t *k, const demod_out_t *o)
 * Per group: descramble, s1 = mul(x, gain), s2 = mul(y, gain), s0 = mul(|x| - |y|, gain0),
 * then each plane's 8 hard and 8 weak bits through ACCX. a7..a9 hard planes, a11..a13 weak. */
    .macro plane8 sa, hard, weak
    bits8 \sa
    s8i a15, \weak, 0
    srli a15, a15, 8
    s8i a15, \hard, 0
    addi \hard, \hard, 1
    addi \weak, \weak, 1
    .endm

    .align 4
    .global demod_8psk_pie
    .type demod_8psk_pie, @function
demod_8psk_pie:
    entry a1, 32
    l32i a11, a7, WEAK0
    l32i a12, a7, WEAK0 + 4
    l32i a13, a7, WEAK0 + 8
    l32i a8, a7, HARD0 + 4
    l32i a9, a7, HARD0 + 8
    l32i a7, a7, HARD0
    movi a10, 14
    wsr.sar a10
    ee.zero.q q4
    srli a4, a4, 3
    loopnez a4, 1f
    addi a10, a6, SEL
    descramble8
    ee.vsubs.s16 q0, q4, q6
    ee.vmax.s16 q0, q0, q6                   /* |x| */
    ee.vsubs.s16 q1, q4, q7
    ee.vmax.s16 q1, q1, q7                   /* |y| */
    ee.vsubs.s16 q0, q0, q1                  /* |x| - |y| */
    ee.vld.128.ip q2, a10, 16                /* gain */
    ee.vmul.s16 q6, q6, q2                   /* s1 */
    ee.vmul.s16 q7, q7, q2                   /* s2 */
    ee.vld.128.ip q2, a10, 48                /* gain0; a10 to WEAK */
    ee.vmul.s16 q0, q0, q2                   /* s0 */
    ee.vld.128.ip q5, a10, 16                /* 16; a10 at the weights */
    mov a14, a10
    plane8 q0, a7, a11
    plane8 q6, a8, a12
    plane8 q7, a9, a13
1:
    retw
    .size demod_8psk_pie, . - demod_8psk_pie
