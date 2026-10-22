/* ESP32-S3 PIE versions of demod.c's release kernels (demod_pie.h); same bytes as the C.
 * Semantics relied on, checked on the board: EE.VMUL.S16 keeps the low 16 bits of
 * floor(x y / 2^SAR); VADDS/VSUBS.S16 saturate; VCMP sets lanes to all ones or zero;
 * VLDBC.16 broadcasts one 16-bit word; VZIP.16 a, b leaves the low halves interleaved in a
 * and the high halves in b; VUNZIP.8 a, b leaves the even bytes in a; VMULAS.U16.ACCX adds
 * the unsigned products to ACCX. Clobbers q0..q7, SAR and ACCX. Windowed ABI: a2..a7. */
    .section .iram1, "ax"

/* demod_run_t offsets */
    .set CO, 0
    .set SO, 16
    .set LR, 32
    .set LI, 48
    .set DR, 64
    .set DI, 80
    .set SEL, 96
    .set TABLE, 224

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

/* One group of 8: descramble, scale, clip, interleave, hard bits. a2 yi, a3 yq, a5 scr,
 * a12 the constants from sel, q4 zero. Leaves the LLRs in q6 and the hard bits in a13. */
    .macro llr_group
    ee.vld.128.ip q0, a2, 16                 /* yi */
    ee.vld.128.ip q1, a3, 16                 /* yq */
    ee.vldbc.16.ip q2, a5, 2                 /* the 8 codes */
    mov a10, a12
    ee.vld.128.ip q3, a10, 16                /* sel even */
    ee.vld.128.ip q5, a10, 16                /* sel high */
    ee.xorq q7, q0, q1
    ee.andq q3, q2, q3
    ee.andq q5, q2, q5
    ee.vcmp.eq.s16 q3, q3, q4                /* even: R even */
    ee.vcmp.eq.s16 q5, q5, q4                /* low: R < 2 */
    ee.andq q2, q7, q3                       /* e */
    ee.xorq q6, q1, q2                       /* a = R odd ? r : i ... */
    ee.xorq q7, q0, q2                       /* b */
    ee.notq q0, q5                           /* negate a where R >= 2 */
    ee.xorq q1, q3, q5                       /* negate b where R is 1 or 2 */
    ee.vld.128.ip q2, a10, 16                /* gain */
    ee.xorq q6, q6, q0
    ee.vsubs.s16 q6, q6, q0
    ee.xorq q7, q7, q1
    ee.vsubs.s16 q7, q7, q1
    ee.vld.128.ip q0, a10, 16                /* 127 */
    ee.vld.128.ip q1, a10, 16                /* -127 */
    ee.vmul.s16 q6, q6, q2
    ee.vmul.s16 q7, q7, q2
    ee.vmin.s16 q6, q6, q0
    ee.vmin.s16 q7, q7, q0
    ee.vld.128.ip q3, a10, 16                /* weights, first 8 bits */
    ee.vmax.s16 q6, q6, q1
    ee.vmax.s16 q7, q7, q1
    ee.vld.128.ip q5, a10, 16                /* weights, last 8 bits */
    ee.vzip.16 q6, q7                        /* a0 b0 a1 b1 .. | a4 b4 .. */
    ee.vld.128.ip q2, a10, 16                /* ones */
    ee.vcmp.lt.s16 q0, q6, q4
    ee.vcmp.lt.s16 q1, q7, q4
    ee.andq q0, q0, q3
    ee.andq q1, q1, q5
    ee.zero.accx
    ee.vmulas.u16.accx q0, q2
    ee.vmulas.u16.accx q1, q2
    ee.vunzip.8 q6, q7                       /* the low bytes: LLRs in order */
    rur.accx_0 a13
    .endm

/* void demod_llr_pie(const int16_t *yi, const int16_t *yq, unsigned n, const uint8_t *scr,
 *                    const demod_run_t *k, const demod_out_t *o) */
    .align 4
    .global demod_llr_pie
    .type demod_llr_pie, @function
demod_llr_pie:
    entry a1, 32
    l32i a8, a7, 0                           /* llr */
    l32i a9, a7, 4                           /* hard */
    movi a10, 14
    wsr.sar a10
    addi a12, a6, SEL
    ee.zero.q q4
    srli a11, a4, 3
    loopnez a11, 1f
    llr_group
    ee.vst.128.ip q6, a8, 16
    srli a14, a13, 8
    s8i a14, a9, 0
    s8i a13, a9, 1
    addi a9, a9, 2
1:
    bbci a4, 2, 2f                           /* 4 symbols left */
    llr_group
    ee.vst.l.64.ip q6, a8, 8
    srli a14, a13, 8
    s8i a14, a9, 0
2:
    retw
    .size demod_llr_pie, . - demod_llr_pie
