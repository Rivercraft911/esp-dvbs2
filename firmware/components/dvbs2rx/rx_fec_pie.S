/* ESP32-S3 PIE versions of the rx_fec.c kernels; rx_fec_internal.h specifies them and
 * the bench (tests/rx_fec_bench) checks that both give the same bytes.
 * Semantics relied on (ESP32-S3 TRM chapter 1, as in components/dvbs2/pie.S):
 *  - 128-bit loads and stores ignore the low 4 address bits; .ld.incp/.st.incp
 *    variants access 16 bytes at as and add 16, after reading the other operands.
 *  - EE.LD.128.USAR.* sets SAR_BYTE = address & 15; EE.SRC.Q* return bytes SAR_BYTE ..
 *    SAR_BYTE + 15 of qs0 (low) followed by qs1. EE.SRC.Q.LD.IP writes them to qs0.
 *  - VADDS/VSUBS saturate; VCMP sets lanes to all ones or zero; VMUL.S8/U8 keep the low
 *    8 bits of (x * y) >> SAR; VMULAS.U8.ACCX adds the 16 unsigned products to ACCX.
 * The coprocessor is enabled lazily by FreeRTOS on first use. Clobbers q0..q7, SAR,
 * SAR_BYTE and ACCX. Windowed ABI: arguments in a2..a7. */
    .section .iram1, "ax", @progbits

/* void rx_pass1_pie(belief a2, reads a3, r a4, qtmp a5, state a6, count a7)
 * q0 min1, q1 min2, q2 q parity, q3 b parity. Each edge starts with its window's
 * blocks in q5 (low) and q4 (high), SAR_BYTE set, and prefetches the next edge's. */
    .align 4
    .global rx_pass1_pie
    .type rx_pass1_pie, @function
rx_pass1_pie:
    entry a1, 32
    movi a10, rx_pie_consts
    movi a8, 23
    l16ui a9, a3, 0
    addi a3, a3, 2
    add a9, a9, a2
    ee.ld.128.usar.ip q5, a9, 16
    ee.vld.128.ip q4, a9, 0
.Lp1_vector:
    ee.vld.128.ip q0, a10, 0                 /* 127 */
    ee.vld.128.ip q1, a10, 0
    ee.zero.q q2
    ee.zero.q q3
    loopnez a7, .Lp1_edges
    ee.src.q.ld.ip q6, a4, 16, q5, q4        /* q5 = b, q6 = r */
    l16ui a9, a3, 0
    add a9, a9, a2
    ee.vsubs.s8 q7, q6, q5                   /* -q */
    ee.vsubs.s8 q6, q5, q6                   /* q */
    ee.xorq q3, q3, q5
    ee.ld.128.usar.ip q5, a9, 16             /* next window, low block */
    ee.vmax.s8 q7, q6, q7                    /* |q| */
    ee.xorq q2, q2, q6
    ee.vmax.s8.st.incp q7, a5, q4, q0, q7    /* q4 = max(min1, |q|); store |q| */
    ee.vmin.s8.st.incp q6, a5, q1, q1, q4    /* min2; store q */
    ee.vmin.s8.ld.incp q4, a9, q0, q0, q7    /* min1; next window, high block */
    addi a3, a3, 2
.Lp1_edges:
    ee.vst.128.ip q0, a6, 16
    ee.vst.128.ip q1, a6, 16
    ee.vst.128.ip q2, a6, 16
    ee.vst.128.ip q3, a6, 16
    addi a8, a8, -1
    bnez a8, .Lp1_vector
    retw
    .size rx_pass1_pie, . - rx_pass1_pie

/* void rx_pass2_pie(belief a2, writes a3, r a4, qtmp a5, state a6, edges a7)
 * q0 zero, q1 min2, q2 n1 signed, q3 n1 ^ n2 signed; each edge starts with |q| in q5
 * and q in q4 and loads the next edge's. a10 = belief + 16 v. */
    .align 4
    .global rx_pass2_pie
    .type rx_pass2_pie, @function
rx_pass2_pie:
    entry a1, 32
    extui a11, a7, 0, 8
    extui a12, a7, 8, 8
    movi a13, 7
    wsr.sar a13
    movi a14, rx_pie_consts + 16             /* 112, then 47 */
    mov a15, a3
    mov a10, a2
    movi a8, 23
    ee.zero.q q0
    ee.vld.128.ip q5, a5, 16
    ee.vld.128.ip q4, a5, 16
.Lp2_vector:
    ee.vld.128.ip q2, a6, 16                 /* min1 */
    ee.vld.128.ip q1, a6, 16                 /* min2 */
    ee.vld.128.ip q3, a6, 32                 /* q parity, skip b parity */
    ee.vld.128.ip q6, a14, 16
    ee.vld.128.ip q7, a14, -16
    ee.vmul.s8 q2, q2, q6                    /* min * 112 >> 7 */
    ee.vmul.s8 q6, q1, q6
    ee.vmin.s8 q2, q2, q7                    /* at most 47 */
    ee.vmin.s8 q6, q6, q7
    ee.vcmp.lt.s8 q3, q3, q0
    ee.xorq q2, q2, q3
    ee.vsubs.s8 q2, q2, q3                   /* s1 */
    ee.xorq q6, q6, q3
    ee.vsubs.s8 q6, q6, q3                   /* s2 */
    ee.xorq q3, q2, q6
    mov a3, a15
    loopnez a11, .Lp2_first
    ee.vcmp.lt.s8 q6, q5, q1                 /* |q| < min2 */
    l16ui a9, a3, 0
    ee.andq q6, q6, q3
    add a9, a9, a10
    ee.xorq q6, q6, q2                       /* s2 or s1 */
    ee.vcmp.lt.s8 q7, q4, q0
    ee.xorq q6, q6, q7
    ee.vsubs.s8.ld.incp q5, a5, q6, q6, q7   /* message; next |q| */
    ee.vadds.s8.st.incp q6, a4, q7, q4, q6   /* belief; store message */
    ee.vst.128.ip q7, a9, 0
    ee.vld.128.ip q4, a5, 16                 /* next q */
    addi a3, a3, 2
.Lp2_first:
    loopnez a12, .Lp2_repeats
    ee.vcmp.lt.s8 q6, q5, q1
    l16ui a9, a3, 0
    ee.andq q6, q6, q3
    add a9, a9, a10
    ee.xorq q6, q6, q2
    ee.vcmp.lt.s8 q7, q4, q0
    ee.xorq q6, q6, q7
    ee.vsubs.s8.ld.incp q5, a5, q6, q6, q7
    ee.vld.128.ip q7, a4, 0                  /* old message */
    ee.vsubs.s8.st.incp q6, a4, q7, q6, q7   /* change; store message */
    ee.vst.128.ip q7, a9, 0
    ee.vld.128.ip q4, a5, 16
    addi a3, a3, 2
.Lp2_repeats:
    addi a10, a10, 16
    addi a8, a8, -1
    bnez a8, .Lp2_vector
    retw
    .size rx_pass2_pie, . - rx_pass2_pie

/* void rx_tails_pie(belief a2, writes a3, count a4) */
    .align 4
    .global rx_tails_pie
    .type rx_tails_pie, @function
rx_tails_pie:
    entry a1, 32
    loopnez a4, .Ltails
    l16ui a5, a3, 0
    addi a3, a3, 2
    add a5, a5, a2
    ee.vld.128.ip q0, a5, 0
    addmi a6, a5, 256
    addi a6, a6, 104
    ee.vst.l.64.ip q0, a6, 8
    ee.vst.h.64.ip q0, a6, 8
.Ltails:
    retw
    .size rx_tails_pie, . - rx_tails_pie

/* Stream the 23 windows of a slot that start at offset 'start' (a 16-byte window
 * at (start + 16 v) mod 360): \body runs with the window in q2 and may use q3, q4.
 * Uses a9..a13. */
    .macro windows slot, start, body
    movi a9, 375
    sub a9, a9, \start
    srli a9, a9, 4                           /* windows before the wrap */
    movi a10, 23
    sub a10, a10, a9
    add a11, \slot, \start
    slli a12, a9, 4
    add a12, a12, a11
    movi a13, 360
    sub a12, a12, a13                        /* first window after the wrap */
    ee.ld.128.usar.ip q0, a11, 16
    loopnez a9, .Lw1\@
    ee.vld.128.ip q1, a11, 16
    ee.src.q.qup q2, q0, q1
    \body
.Lw1\@:
    ee.ld.128.usar.ip q0, a12, 16
    loopnez a10, .Lw2\@
    ee.vld.128.ip q1, a12, 16
    ee.src.q.qup q2, q0, q1
    \body
.Lw2\@:
    .endm

/* void rx_repeat_pie(home a2, scratch a3, start a4) */
    .macro repeat_body
    ee.vld.128.ip q3, a5, 0
    ee.vadds.s8 q2, q2, q3
    ee.vst.128.ip q2, a5, 16
    .endm
    .align 4
    .global rx_repeat_pie
    .type rx_repeat_pie, @function
rx_repeat_pie:
    entry a1, 32
    mov a5, a3
    windows a2, a4, repeat_body
    mov a5, a3
    mov a6, a2
    movi a7, 23
    loopnez a7, .Lrepeat_copy
    ee.vld.128.ip q0, a5, 16
    ee.vst.128.ip q0, a6, 16
.Lrepeat_copy:
    ee.vld.128.ip q0, a2, 0
    addmi a6, a2, 256
    addi a6, a6, 104
    ee.vst.l.64.ip q0, a6, 8
    ee.vst.h.64.ip q0, a6, 8
    retw
    .size rx_repeat_pie, . - rx_repeat_pie

/* void rx_scale_pie(belief a2, slots a3, gain a4)
 * |x| is clamped to ceil(1024 / gain) first so the product >> 4 fits a byte (< 69)
 * and still saturates to 63 exactly when the full product would. */
    .align 4
    .global rx_scale_pie
    .type rx_scale_pie, @function
rx_scale_pie:
    entry a1, 48
    movi a5, 1023
    add a5, a5, a4
    quou a5, a5, a4
    movi a6, 127
    minu a5, a5, a6
    s8i a5, a1, 16
    s8i a4, a1, 17
    addi a6, a1, 16
    ee.vldbc.8 q4, a6                        /* cap */
    addi a6, a1, 17
    ee.vldbc.8 q5, a6                        /* gain */
    movi a6, rx_pie_consts + 48
    ee.vld.128.ip q6, a6, 0                  /* 63 */
    ee.zero.q q7
    movi a6, 4
    wsr.sar a6
    movi a6, 24
    mull a3, a3, a6
    loopnez a3, .Lscale
    ee.vld.128.ip q0, a2, 0
    ee.vsubs.s8 q1, q7, q0
    ee.vmax.s8 q1, q1, q0                    /* min(|x|, 127) */
    ee.vmin.s8 q1, q1, q4
    ee.vmul.u8 q1, q1, q5
    ee.vmin.s8 q1, q1, q6
    ee.vcmp.lt.s8 q2, q0, q7
    ee.xorq q1, q1, q2
    ee.vsubs.s8 q1, q1, q2
    ee.vst.128.ip q1, a2, 16
.Lscale:
    retw
    .size rx_scale_pie, . - rx_scale_pie

/* void rx_moments_pie(llr a2 (any alignment), vectors a3, sums a4): one pass adds |x|
 * into ACCX, a second |x|^2. */
    .macro moments_pass square
    mov a7, a2
    ee.zero.accx
    ee.ld.128.usar.ip q0, a7, 16
    loopnez a3, .Lm\@
    ee.vld.128.ip q1, a7, 16
    ee.src.q.qup q2, q0, q1
    ee.vsubs.s8 q3, q7, q2
    ee.vmax.s8 q3, q3, q2
    .if \square
    ee.vmulas.u8.accx q3, q3
    .else
    ee.vmulas.u8.accx q3, q5
    .endif
.Lm\@:
    ee.st.accx.ip a6, 0
    l32i a8, a6, 0
    s32i a8, a4, 4 * \square
    .endm
    .align 4
    .global rx_moments_pie
    .type rx_moments_pie, @function
rx_moments_pie:
    entry a1, 48
    movi a5, rx_pie_consts + 64
    ee.vld.128.ip q5, a5, 0                  /* ones */
    ee.zero.q q7
    addi a6, a1, 16
    moments_pass 0
    moments_pass 1
    retw
    .size rx_moments_pie, . - rx_moments_pie

/* int rx_parity_pie(belief a2, checks a3, count a4): XOR of the windows of every edge
 * into x[23] on the stack, then the sign bits of lanes 0..359. */
    .macro parity_body
    ee.vld.128.ip q3, a14, 0
    ee.xorq q2, q2, q3
    ee.vst.128.ip q2, a14, 16
    .endm
    .align 4
    .global rx_parity_pie
    .type rx_parity_pie, @function
rx_parity_pie:
    entry a1, 416
    addi a14, a1, 32
    ee.zero.q q2
    movi a5, 23
    loopnez a5, .Lparity_zero
    ee.vst.128.ip q2, a14, 16
.Lparity_zero:
.Lparity_edge:
    l16ui a6, a3, 0
    l16ui a7, a3, 2
    addi a3, a3, 4
    add a6, a6, a2
    addi a14, a1, 32
    windows a6, a7, parity_body
    addi a4, a4, -1
    bnez a4, .Lparity_edge
    addi a14, a1, 32
    ee.zero.q q0
    movi a5, 22
    loopnez a5, .Lparity_or
    ee.vld.128.ip q1, a14, 16
    ee.orq q0, q0, q1
.Lparity_or:
    ee.movi.32.a q0, a8, 0
    ee.movi.32.a q0, a9, 1
    or a8, a8, a9
    ee.movi.32.a q0, a9, 2
    or a8, a8, a9
    ee.movi.32.a q0, a9, 3
    or a8, a8, a9
    l32i a9, a14, 0                          /* last vector, lanes 0..7 */
    or a8, a8, a9
    l32i a9, a14, 4
    or a8, a8, a9
    movi a9, 0x80808080
    and a2, a8, a9
    retw
    .size rx_parity_pie, . - rx_parity_pie

/* void rx_hard_pie(belief a2, frames a3, groups a4, hard a5): signs masked by bit
 * weights, each half of a window summed into ACCX for one byte. */
    .macro hard_body
    ee.vcmp.lt.s8 q2, q2, q7
    ee.andq q2, q2, q6
    ee.zero.accx
    ee.vmulas.u8.accx q2, q4
    ee.st.accx.ip a15, 0
    l8ui a8, a15, 0
    s8i a8, a5, 0
    ee.zero.accx
    ee.vmulas.u8.accx q2, q5
    ee.st.accx.ip a15, 0
    l8ui a8, a15, 0
    s8i a8, a5, 1
    addi a5, a5, 2
    .endm
    .align 4
    .global rx_hard_pie
    .type rx_hard_pie, @function
rx_hard_pie:
    entry a1, 48
    movi a6, rx_pie_consts + 80
    ee.vld.128.ip q6, a6, 16                 /* bit weights */
    ee.vld.128.ip q4, a6, 16                 /* ones in lanes 0..7 */
    ee.vld.128.ip q5, a6, 0                  /* ones in lanes 8..15 */
    ee.zero.q q7
    addi a15, a1, 16
    mov a14, a2
.Lhard_group:
    l16ui a7, a3, 0
    addi a3, a3, 2
    windows a14, a7, hard_body
    addi a5, a5, -1                          /* 46 bytes written, 45 kept */
    movi a8, 384
    add a14, a14, a8
    addi a4, a4, -1
    bnez a4, .Lhard_group
    retw
    .size rx_hard_pie, . - rx_hard_pie

    .section .dram1, "aw", @progbits
    .balign 16
    .global rx_pie_consts
rx_pie_consts:
    .fill 16, 1, 127
    .fill 16, 1, 112
    .fill 16, 1, 47
    .fill 16, 1, 63
    .fill 16, 1, 1
    .byte 128, 64, 32, 16, 8, 4, 2, 1, 128, 64, 32, 16, 8, 4, 2, 1
    .fill 8, 1, 1
    .fill 8, 1, 0
    .fill 8, 1, 0
    .fill 8, 1, 1
