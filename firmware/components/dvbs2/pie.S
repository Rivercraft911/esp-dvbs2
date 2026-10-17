/* ESP32-S3 PIE kernels for fast.c. Same data layouts as the portable C there.
 * ESP32-S3 TRM v1.8 chapter 1. Notes on semantics used here:
 *  - 128-bit loads and stores ignore the low 4 address bits.
 *  - EE.LD.128.USAR.* sets SAR_BYTE = address & 15; EE.SRC.Q qa, qs0, qs1 gives
 *    bytes SAR_BYTE .. SAR_BYTE + 15 of qs0 (low) followed by qs1 (high).
 *  - EE.VSR.32 is arithmetic; every use below masks or discards the sign bits.
 *  - EE.VMUL.U8 keeps the low 8 bits of (x * y) >> SAR per byte, so with y = 2
 *    it shifts each byte left by one (SAR 0) or right by seven (SAR 8).
 *  - EE.LDXQ.32 address: the TRM says as + 4 * index; QEMU uses as + 4 * index
 *    - 4. fast.c measures it with dvbs2_pie_ldxq and passes a corrected table.
 * Windowed ABI: arguments in a2..a7. */

    .section .iram1, "ax"

/* uint32_t dvbs2_pie_ldxq(const uint32_t *table): EE.LDXQ.32 with index 1. */
    .align 4
    .global dvbs2_pie_ldxq
    .type dvbs2_pie_ldxq, @function
dvbs2_pie_ldxq:
    entry a1, 32
    movi a3, 1
    ee.zero.q q0
    ee.movi.32.q q0, a3, 0
    ee.zero.q q1
    ee.ldxq.32 q1, q0, a2, 0, 0
    ee.movi.32.a q1, a2, 0
    retw
    .size dvbs2_pie_ldxq, . - dvbs2_pie_ldxq

/* void dvbs2_pie_xor(uint8_t *dst, const uint8_t *src, const uint8_t *prbs,
 *                    unsigned blocks)
 * dst[i] = src[i] ^ prbs[i] for 16 blocks bytes. dst and prbs 16-byte aligned,
 * src any; reads the aligned blocks covering src .. src + 16 blocks + 15. */
    .align 4
    .global dvbs2_pie_xor
    .type dvbs2_pie_xor, @function
dvbs2_pie_xor:
    entry a1, 32
    ee.ld.128.usar.ip q0, a3, 16
    loopnez a5, 1f
    ee.vld.128.ip q1, a3, 16
    ee.vld.128.ip q3, a4, 16
    ee.src.q.qup q2, q0, q1             /* q2 = source bytes, q0 = q1 */
    ee.xorq q2, q2, q3
    ee.vst.128.ip q2, a2, 16
1:
    retw
    .size dvbs2_pie_xor, . - dvbs2_pie_xor

/* BCH: eight four-bit slices per 32-bit message word. Each slice is 512
 * bytes: sixteen 16-byte low rows, then sixteen 16-byte high rows. The
 * complete table is 4096 bytes and fits alongside the other cached tables. */
    .macro bch_nibble shift, high
    extui a8, a6, \shift, 4
    slli a8, a8, 4
    add a8, a8, a4
    ee.vld.128.xp q2, a8, a13
    .if \high
    ee.vld.128.ip q3, a8, 0
    .endif
    ee.xorq q0, q0, q2
    .if \high
    ee.xorq q1, q1, q3
    .endif
    add a4, a4, a9
    .endm

    .macro bch_kernel name, high
    .align 4
    .global \name
    .type \name, @function
\name:
    entry a1, 32
    ee.zero.q q0
    ee.zero.q q1
    .if !\high
    ee.zero.q q7
    .endif
    mov a10, a4
    movi a9, 512
    movi a13, 256
    loopnez a3, 99f
    l32i a6, a2, 0
    ee.movi.32.a q0, a7, 0
    addi a2, a2, 4
    xor a6, a6, a7
    .if \high
    ee.srci.2q q1, q0, 3
    .else
    ee.srci.2q q7, q0, 3
    .endif
    mov a4, a10
    bch_nibble 4, \high
    bch_nibble 0, \high
    bch_nibble 12, \high
    bch_nibble 8, \high
    bch_nibble 20, \high
    bch_nibble 16, \high
    bch_nibble 28, \high
    bch_nibble 24, \high
99:
    ee.vst.128.ip q0, a5, 16
    ee.vst.128.ip q1, a5, 16
    retw
    .size \name, . - \name
    .endm
    bch_kernel dvbs2_pie_bch32, 1
    bch_kernel dvbs2_pie_bch16, 0

/* The local pointer is below the 48-byte window-spill save area.
 * Same nibble rows, with bases resident in a4/a9..a15. For all but
 * the low nibble, EXTUI can leave four junk low address bits: VLD ignores
 * them (TRM 1.8.88-89, pp. 164-165). This removes seven shifts and eight base increments
 * per word without increasing the 4 KiB table. */
    .macro bch_reg_slice shift, base, high, first=0
    .if \shift
    extui a8, a6, (\shift - 4), 8
    .else
    extui a8, a6, 0, 4
    slli a8, a8, 4
    .endif
    .if !\high && !\first
    ee.xorq q0, q0, q2                 /* previous load, latency hidden */
    .endif
    add a8, a8, \base
    ee.vld.128.xp q2, a8, a5
    .if \high
    ee.vld.128.ip q3, a8, 0
    ee.xorq q0, q0, q2
    ee.xorq q1, q1, q3
    .endif
    .endm

    .macro bch_regs name, high
    .align 4
    .global \name
    .type \name, @function
\name:
    entry a1, 80
    s32i a5, a1, 16
    movi a5, 256
    addmi a9, a4, 512
    addmi a10, a4, 1024
    addmi a11, a4, 1536
    addmi a12, a4, 2048
    addmi a13, a4, 2560
    addmi a14, a4, 3072
    addmi a15, a4, 3584
    ee.zero.q q0
    ee.zero.q q1
    .if !\high
    ee.zero.q q7
    .endif
    loopnez a3, 99f
    l32i a6, a2, 0
    ee.movi.32.a q0, a7, 0
    xor a6, a6, a7
    .if \high
    ee.srci.2q q1, q0, 3
    .else
    ee.srci.2q q7, q0, 3
    .endif
    bch_reg_slice 4, a4, \high, 1
    bch_reg_slice 0, a9, \high
    bch_reg_slice 12, a10, \high
    bch_reg_slice 8, a11, \high
    bch_reg_slice 20, a12, \high
    bch_reg_slice 16, a13, \high
    bch_reg_slice 28, a14, \high
    bch_reg_slice 24, a15, \high
    addi a2, a2, 4
    .if !\high
    ee.xorq q0, q0, q2                 /* final load, pointer update fills the gap */
    .endif
99:
    l32i a5, a1, 16
    ee.vst.128.ip q0, a5, 16
    ee.vst.128.ip q1, a5, 16
    retw
    .size \name, . - \name
    .endm
    bch_regs dvbs2_pie_bch_regs32, 1
    bch_regs dvbs2_pie_bch_regs16, 0

/* Byte recurrence R' = (R << 8) XOR T[(R >> (r-8)) XOR byte],
 * with the canonical MSB-first byte string in q0:q1. T[v] = v*x^r mod g.
 * One 8 KiB table, low halves then high halves; no per-word byte reversal.
 * The last partial word is included. Same arguments, count is bytes. */
    .macro bch_byte_step offset
    l8ui a6, a2, \offset
    ee.movi.32.a q0, a7, 0
    xor a6, a6, a7
    extui a8, a6, 0, 8
    slli a8, a8, 4
    add a8, a8, a4
    ee.srci.2q q1, q0, 0
    ee.vld.128.xp q2, a8, a9
    ee.vld.128.ip q3, a8, 0
    ee.xorq q0, q0, q2
    ee.xorq q1, q1, q3
    .endm
    .align 4
    .global dvbs2_pie_bch_byte
    .type dvbs2_pie_bch_byte, @function
dvbs2_pie_bch_byte:
    entry a1, 32
    ee.zero.q q0
    ee.zero.q q1
    movi a9, 4096
    srli a10, a3, 2
    extui a3, a3, 0, 2
    loopnez a10, 99f
    bch_byte_step 0
    bch_byte_step 1
    bch_byte_step 2
    bch_byte_step 3
    addi a2, a2, 4
99:
    loopnez a3, 98f
    bch_byte_step 0
    addi a2, a2, 1
98:
    ee.vst.128.ip q0, a5, 16
    ee.vst.128.ip q1, a5, 16
    retw
    .size dvbs2_pie_bch_byte, . - dvbs2_pie_bch_byte

/* Copies a BCH table into the LDPC scratch which is dead until BCH ends.
 * 8192 bytes, no additional permanent internal allocation. */
    .align 4
    .global dvbs2_pie_bch_copy
    .type dvbs2_pie_bch_copy, @function
dvbs2_pie_bch_copy:
    entry a1, 32
    movi a4, 512
    loop a4, 99f
    ee.vld.128.ip q0, a3, 16
    ee.vst.128.ip q0, a2, 16
99:
    retw
    .size dvbs2_pie_bch_copy, . - dvbs2_pie_bch_copy

/* LDPC doubled groups: u2 = u u u[0..5] per 360-bit group u (45 bytes), 96
 * bytes each. U0 = u[0..15], U1 = u[16..31], U2 = u[32..47], X = u[29..44];
 * u2 blocks: U0, U1, {X,U0}>>3, {U0,U1}>>3, {U1,U2}>>3, {X,U0}>>6 (bytes).
 * void dvbs2_pie_u2(const uint8_t *bits, uint8_t *u2, unsigned groups) */
    .align 4
    .global dvbs2_pie_u2
    .type dvbs2_pie_u2, @function
dvbs2_pie_u2:
    entry a1, 32
    movi a8, 3
    movi a9, 6
    loopnez a4, 4f
    mov a5, a2
    ee.ld.128.usar.ip q0, a5, 16
    ee.vld.128.ip q1, a5, 16
    ee.vld.128.ip q2, a5, 16
    ee.vld.128.ip q3, a5, 0
    addi a6, a2, 29
    ee.src.q q4, q0, q1                 /* U0 */
    ee.src.q q5, q1, q2                 /* U1 */
    ee.src.q q6, q2, q3                 /* U2 */
    ee.vst.128.ip q4, a3, 16
    ee.ld.128.usar.ip q0, a6, 16
    ee.vld.128.ip q1, a6, 0
    ee.vst.128.ip q5, a3, 16
    ee.src.q q7, q0, q1                 /* X */
    wur.sar_byte a8
    ee.src.q q0, q7, q4                 /* u[32..44] u[0..2] */
    ee.src.q q1, q4, q5                 /* u[3..18] */
    ee.src.q q2, q5, q6                 /* u[19..34] */
    wur.sar_byte a9
    ee.src.q q3, q7, q4                 /* u[35..44] u[0..5] */
    ee.vst.128.ip q0, a3, 16
    ee.vst.128.ip q1, a3, 16
    ee.vst.128.ip q2, a3, 16
    ee.vst.128.ip q3, a3, 16
    addi a2, a2, 45
4:
    retw
    .size dvbs2_pie_u2, . - dvbs2_pie_u2

/* LDPC parity rows (Horner): H = q0..q2 (48 bytes). Program per row: levels,
 * then per level a window count and u2 byte offsets. Between levels H is read
 * one bit further: byte j = (H[j] << 1) | (H[j + 1] >> 7), using EE.VMUL.U8
 * by 2 (q7) with SAR 0 and 8. After the row: H ^= previous row (accumulation
 * down the rows), store to p.
 * void dvbs2_pie_rows(const uint8_t *u2, const uint16_t *prog, uint8_t *p,
 *                     unsigned q) */
    .align 4
    .global dvbs2_pie_rows
    .type dvbs2_pie_rows, @function
dvbs2_pie_rows:
    entry a1, 32
    movi a9, 0x02020202
    ee.movi.32.q q7, a9, 0
    ee.movi.32.q q7, a9, 1
    ee.movi.32.q q7, a9, 2
    ee.movi.32.q q7, a9, 3
    movi a9, 1                          /* SAR_BYTE for the one-byte neighbour */
    movi a10, 0                         /* rows done */
5:  /* row */
    ee.zero.q q0
    ee.zero.q q1
    ee.zero.q q2
    l16ui a6, a3, 0                     /* levels */
    addi a3, a3, 2
6:  /* level */
    l16ui a7, a3, 0                     /* windows */
    addi a3, a3, 2
    loopnez a7, 7f
    l16ui a8, a3, 0
    addi a3, a3, 2
    add a8, a8, a2
    ee.ld.128.usar.ip q3, a8, 16
    ee.vld.128.ip q4, a8, 16
    ee.src.q.ld.ip q5, a8, 16, q3, q4  /* W0 and next aligned load */
    ee.xorq q0, q0, q3
    ee.src.q.ld.ip q6, a8, 0, q4, q5   /* W1 and final aligned load */
    ee.xorq q1, q1, q4
    ee.src.q q5, q5, q6                 /* W2 */
    ee.xorq q2, q2, q5
7:
    addi a6, a6, -1
    beqz a6, 8f
    wur.sar_byte a9
    ee.src.q q3, q0, q1                 /* H bytes 1..16 */
    ee.src.q q4, q1, q2                 /* 17..32 */
    ee.src.q q5, q2, q2                 /* 33..47, then junk */
    ssai 0
    ee.vmul.u8 q0, q0, q7
    ee.vmul.u8 q1, q1, q7
    ee.vmul.u8 q2, q2, q7
    ssai 8
    ee.vmul.u8 q3, q3, q7
    ee.vmul.u8 q4, q4, q7
    ee.vmul.u8 q5, q5, q7
    ee.orq q0, q0, q3
    ee.orq q1, q1, q4
    ee.orq q2, q2, q5
    j 6b
8:
    beqz a10, 9f
    addi a11, a4, -48
    ee.vld.128.ip q3, a11, 16
    ee.vld.128.ip q4, a11, 16
    ee.vld.128.ip q5, a11, 0
    ee.xorq q0, q0, q3
    ee.xorq q1, q1, q4
    ee.xorq q2, q2, q5
9:
    ee.vst.128.ip q0, a4, 16
    ee.vst.128.ip q1, a4, 16
    ee.vst.128.ip q2, a4, 16
    addi a10, a10, 1
    bne a10, a5, 5b
    retw
    .size dvbs2_pie_rows, . - dvbs2_pie_rows

/* 32x32 bit transposes of parity rows, four 32-bit columns of words per
 * 128-bit register (Hacker's Delight transpose32: afterwards word k bit i =
 * old word 31 - i bit 31 - k). Each block: 32 rows of 48 bytes, three column
 * quads. The carry row (48 bytes) is XORed into every row on the first pass.
 * Stage pairs: (16, 8) on rows {k, k+8, k+16, k+24}, (4, 2) on
 * {k, k+2, k+4, k+6}, then 1 on {k, k+1}.
 * void dvbs2_pie_transpose(uint8_t *p, unsigned blocks, const uint8_t *carry) */

/* One delta swap: t = (lo ^ (hi >> j)) & mask; lo ^= t; hi ^= t << j.
 * SAR = j already set; tmp is clobbered. */
    .macro swap lo, hi, mask, tmp
    ee.vsr.32 \tmp, \hi
    ee.xorq \tmp, \tmp, \lo
    ee.andq \tmp, \tmp, \mask
    ee.xorq \lo, \lo, \tmp
    ee.vsl.32 \tmp, \tmp
    ee.xorq \hi, \hi, \tmp
    .endm

    .macro splat q, value
    movi a15, \value
    ee.movi.32.q \q, a15, 0
    ee.movi.32.q \q, a15, 1
    ee.movi.32.q \q, a15, 2
    ee.movi.32.q \q, a15, 3
    .endm

    .align 4
    .global dvbs2_pie_transpose
    .type dvbs2_pie_transpose, @function
dvbs2_pie_transpose:
    entry a1, 32
    movi a12, 384                       /* 8 rows */
    movi a13, 96                        /* 2 rows */
    movi a14, 48                        /* 1 row */
    movi a11, 288                       /* 6 rows */
    beqz a3, 19f
10: /* block */
    movi a10, 0                         /* column quad offset 0, 16, 32 */
11: /* column quad */
    add a5, a2, a10                     /* row 0 of this quad */
    add a6, a4, a10
    ee.vld.128.ip q7, a6, 0             /* carry words */
    splat q6, 0x0000ffff
    splat q5, 0x00ff00ff
    /* pass A: stages 16 and 8 */
    movi a7, 8
12:
    mov a8, a5
    ee.vld.128.xp q0, a8, a12           /* row k */
    ee.vld.128.xp q1, a8, a12           /* k + 8 */
    ee.vld.128.xp q2, a8, a12           /* k + 16 */
    ee.vld.128.ip q3, a8, 0             /* k + 24 */
    ee.xorq q0, q0, q7
    ee.xorq q1, q1, q7
    ee.xorq q2, q2, q7
    ee.xorq q3, q3, q7
    ssai 16
    swap q0, q2, q6, q4
    swap q1, q3, q6, q4
    ssai 8
    swap q0, q1, q5, q4
    swap q2, q3, q5, q4
    mov a8, a5
    ee.vst.128.xp q0, a8, a12
    ee.vst.128.xp q1, a8, a12
    ee.vst.128.xp q2, a8, a12
    ee.vst.128.ip q3, a8, 0
    addi a5, a5, 48
    addi a7, a7, -1
    bnez a7, 12b
    /* pass B: stages 4 and 2 on k = 0, 1, 8, 9, 16, 17, 24, 25 */
    splat q6, 0x0f0f0f0f
    splat q5, 0x33333333
    add a5, a2, a10
    movi a7, 8
13:
    mov a8, a5
    ee.vld.128.xp q0, a8, a13           /* k */
    ee.vld.128.xp q1, a8, a13           /* k + 2 */
    ee.vld.128.xp q2, a8, a13           /* k + 4 */
    ee.vld.128.ip q3, a8, 0             /* k + 6 */
    ssai 4
    swap q0, q2, q6, q4
    swap q1, q3, q6, q4
    ssai 2
    swap q0, q1, q5, q4
    swap q2, q3, q5, q4
    mov a8, a5
    ee.vst.128.xp q0, a8, a13
    ee.vst.128.xp q1, a8, a13
    ee.vst.128.xp q2, a8, a13
    ee.vst.128.ip q3, a8, 0
    /* next k: +1 row, or after an odd k on to k + 7 */
    addi a5, a5, 48
    bbci a7, 0, 14f
    add a5, a5, a11
14:
    addi a7, a7, -1
    bnez a7, 13b
    /* pass C: stage 1 on rows (k, k + 1) */
    splat q6, 0x55555555
    ssai 1
    add a5, a2, a10
    movi a7, 16
15:
    mov a8, a5
    ee.vld.128.xp q0, a8, a14
    ee.vld.128.ip q1, a8, 0
    swap q0, q1, q6, q4
    mov a8, a5
    ee.vst.128.xp q0, a8, a14
    ee.vst.128.ip q1, a8, 0
    addi a5, a5, 96
    addi a7, a7, -1
    bnez a7, 15b
    addi a10, a10, 16
    movi a7, 48
    bne a10, a7, 11b
    movi a7, 1536                       /* 32 rows */
    add a2, a2, a7
    addi a3, a3, -1
    bnez a3, 10b
19:
    retw
    .size dvbs2_pie_transpose, . - dvbs2_pie_transpose

/* 8PSK index pass, 128 symbols per chunk. Columns a, b, c (any alignment;
 * a and c swapped by the caller for rate 3/5), Gold planes Z0 at gold, Z1 at
 * gold + 2720. Bit planes: P0' = a^b^c (inverted p0), P1 = a ^ Z0,
 * P2 = b ^ (a & (b ^ c)) ^ Z1 ^ (a & Z0). Per 4-symbol group a 16-bit index
 * P0' nibble | P2 nibble << 4 | P1 nibble << 8 (first symbol in bit 3 of each
 * nibble), written in natural group order: zip bytes of P0'/P1 and of
 * P2/zero into 16-bit lanes, pick high and low nibbles with masks
 * 0x0f0f (q6) and 0x00f0 (q7), zip the two groups of each byte.
 * void dvbs2_pie_index8(const uint8_t *a, const uint8_t *b, const uint8_t *c,
 *                       const uint8_t *gold, uint16_t *index, unsigned chunks) */
    .align 4
    .global dvbs2_pie_index8
    .type dvbs2_pie_index8, @function
dvbs2_pie_index8:
    entry a1, 32
    splat q6, 0x0f0f0f0f
    splat q7, 0x00f000f0
    ssai 4
    movi a9, 2720
    add a9, a9, a5                      /* Z1 */
    loopnez a7, 16f
    ee.ld.128.usar.ip q0, a2, 16
    ee.vld.128.ip q5, a2, 0
    ee.src.q q0, q0, q5                 /* A */
    ee.ld.128.usar.ip q1, a3, 16
    ee.vld.128.ip q5, a3, 0
    ee.src.q q1, q1, q5                 /* B */
    ee.ld.128.usar.ip q2, a4, 16
    ee.vld.128.ip q5, a4, 0
    ee.vld.128.ip q3, a5, 16            /* Z0 */
    ee.vld.128.ip q4, a9, 16            /* Z1 */
    ee.src.q q2, q2, q5                 /* C */
    ee.xorq q5, q1, q2                  /* B ^ C */
    ee.xorq q2, q0, q5                  /* P0' */
    ee.andq q5, q0, q5
    ee.xorq q1, q1, q5                  /* B ^ (A & (B ^ C)) */
    ee.andq q5, q0, q3
    ee.xorq q0, q0, q3                  /* P1 */
    ee.xorq q1, q1, q4
    ee.xorq q1, q1, q5                  /* P2 */
    ee.vzip.8 q2, q0                    /* 16-bit lanes P0' | P1 << 8 */
    ee.zero.q q3
    ee.vzip.8 q1, q3                    /* 16-bit lanes P2 */
    /* bytes 0..7: groups 0..15 */
    ee.vsr.32 q4, q2
    ee.andq q5, q1, q7
    ee.andq q4, q4, q6
    ee.vsl.32 q1, q1
    ee.orq q4, q4, q5                   /* first group of each byte */
    ee.andq q2, q2, q6
    ee.andq q1, q1, q7
    ee.orq q2, q2, q1                   /* second group */
    ee.vzip.16 q4, q2
    ee.vst.128.ip q4, a6, 16
    ee.vst.128.ip q2, a6, 16
    /* bytes 8..15: groups 16..31 */
    ee.vsr.32 q4, q0
    ee.andq q5, q3, q7
    ee.andq q4, q4, q6
    ee.vsl.32 q3, q3
    ee.orq q4, q4, q5
    ee.andq q0, q0, q6
    ee.andq q3, q3, q7
    ee.orq q0, q0, q3
    ee.vzip.16 q4, q0
    ee.vst.128.ip q4, a6, 16
    ee.vst.128.ip q0, a6, 16
16:
    retw
    .size dvbs2_pie_index8, . - dvbs2_pie_index8

/* QPSK index pass, 64 symbols per chunk. x: FEC bytes (aligned), gold: one
 * byte per four symbols with Z1 at bit 7 - 2i and Z0 at bit 6 - 2i. With
 * D = x ^ (x >> 1): p1 = D ^ g and p2 = x ^ (g >> 1) ^ (D & g) at the Q bit
 * positions (mask 0x55); index byte = p1 | p2 << 1, widened to 16 bits.
 * void dvbs2_pie_index4(const uint8_t *x, const uint8_t *gold, uint16_t *index,
 *                       unsigned chunks) */
    .align 4
    .global dvbs2_pie_index4
    .type dvbs2_pie_index4, @function
dvbs2_pie_index4:
    entry a1, 32
    splat q7, 0x55555555
    ssai 1
    loopnez a5, 17f
    ee.vld.128.ip q0, a2, 16            /* x */
    ee.vld.128.ip q1, a3, 16            /* g */
    ee.vsr.32 q2, q0
    ee.vsr.32 q4, q1
    ee.xorq q2, q2, q0                  /* D */
    ee.andq q3, q2, q1                  /* D & g */
    ee.xorq q2, q2, q1                  /* p1 */
    ee.xorq q0, q0, q4
    ee.xorq q0, q0, q3                  /* p2 */
    ee.andq q2, q2, q7
    ee.andq q0, q0, q7
    ee.vsl.32 q0, q0
    ee.orq q2, q2, q0
    ee.zero.q q3
    ee.vzip.8 q2, q3
    ee.vst.128.ip q2, a4, 16
    ee.vst.128.ip q3, a4, 16
17:
    retw
    .size dvbs2_pie_index4, . - dvbs2_pie_index4

/* One data run: table lookups, four groups (16 phase bytes) per vector,
 * stored through EE.SRCQ.128.ST.INCP so the run may start at any address.
 * block = (dst - 1) & ~15, sar_byte = -dst & 15. Store i writes vector i - 1's
 * tail and vector i's head to block + 16 i; the final store goes to tail
 * (16 bytes, aligned) for the caller to copy.
 * void dvbs2_pie_run(const uint16_t *index, const uint32_t *table,
 *                    uint8_t *block, unsigned vectors, unsigned sar_byte,
 *                    uint8_t *tail) */
    .align 4
    .global dvbs2_pie_run
    .type dvbs2_pie_run, @function
dvbs2_pie_run:
    entry a1, 32
    wur.sar_byte a6
    ee.zero.q q0                        /* vector -1 */
    srli a8, a5, 1
    ee.vld.128.ip q2, a2, 16            /* 8 group indices, one pair ahead */
    loopnez a8, 18f
    ee.ldxq.32 q1, q2, a3, 0, 0
    ee.ldxq.32 q1, q2, a3, 1, 1
    ee.ldxq.32 q1, q2, a3, 2, 2
    ee.ldxq.32 q1, q2, a3, 3, 3
    ee.ldxq.32 q3, q2, a3, 0, 4
    ee.ldxq.32 q3, q2, a3, 1, 5
    ee.srcq.128.st.incp q0, q1, a4
    ee.ldxq.32 q3, q2, a3, 2, 6
    ee.ldxq.32 q3, q2, a3, 3, 7
    ee.vld.128.ip q2, a2, 16
    ee.srcq.128.st.incp q1, q3, a4
    ee.orq q0, q3, q3
18:
    bbci a5, 0, 20f
    ee.ldxq.32 q1, q2, a3, 0, 0         /* odd vector count: one more */
    ee.ldxq.32 q1, q2, a3, 1, 1
    ee.ldxq.32 q1, q2, a3, 2, 2
    ee.ldxq.32 q1, q2, a3, 3, 3
    ee.srcq.128.st.incp q0, q1, a4
    ee.orq q0, q1, q1
20:
    ee.srcq.128.st.incp q0, q1, a7
    retw
    .size dvbs2_pie_run, . - dvbs2_pie_run

/* 8PSK: the index has three four-bit planes at offsets 0, 4, 8.
 * A two-symbol lookup keeps its planes at offsets 0, 4, 8 too: mask 0x333,
 * instead of repacking to six consecutive bits. The sparse table occupies
 * 4 KiB in flash, saving seventeen instructions per 32 symbols. Same shifted
 * stores as dvbs2_pie_run. */
    .macro pair_lookup lane
    ee.ldxq.32 q1, q0, a3, 0, (\lane + 0)
    ee.ldxq.32 q3, q2, a3, 0, (\lane + 0)
    ee.ldxq.32 q1, q0, a3, 1, (\lane + 1)
    ee.ldxq.32 q3, q2, a3, 1, (\lane + 1)
    ee.ldxq.32 q1, q0, a3, 2, (\lane + 2)
    ee.ldxq.32 q3, q2, a3, 2, (\lane + 2)
    ee.ldxq.32 q1, q0, a3, 3, (\lane + 3)
    ee.ldxq.32 q3, q2, a3, 3, (\lane + 3)
    ee.vzip.16 q1, q3
    ee.vunzip.32 q1, q3
    .endm
    .align 4
    .global dvbs2_pie_run8
    .type dvbs2_pie_run8, @function
dvbs2_pie_run8:
    entry a1, 32
    wur.sar_byte a6
    splat q7, 0x03330333
    ee.zero.q q4
    ssai 2
    srli a8, a5, 1
    loopnez a8, 98f
    ee.vld.128.ip q2, a2, 16
    ee.vsr.32 q0, q2
    ee.andq q0, q0, q7
    ee.andq q2, q2, q7
    pair_lookup 0
    ee.srcq.128.st.incp q4, q1, a4
    ee.orq q4, q1, q1
    pair_lookup 4
    ee.srcq.128.st.incp q4, q1, a4
    ee.orq q4, q1, q1
98:
    /* All 8PSK runs are multiples of 32 symbols (1440 or 21600). */
    ee.srcq.128.st.incp q4, q1, a7
    retw
    .size dvbs2_pie_run8, . - dvbs2_pie_run8

/* Four-symbol indices to two packed phase bytes. Index bit 11 toggles output
 * bit 1 and is handled by XOR, halving the table to 8 KiB. Each table word
 * contains a halfword; VUNZIP.16 discards the zero high halves. One lookup per four
 * symbols, rather than two lookups plus a phase-byte packing pass.
 * Same shifted-store contract as run8, vectors = symbols / 32. */
    .align 4
    .global dvbs2_pie_run8_packed
    .type dvbs2_pie_run8_packed, @function
dvbs2_pie_run8_packed:
    entry a1, 32
    wur.sar_byte a6
    ee.zero.q q4
    splat q6, 0x07ff07ff
    splat q7, 0x00020002
    ssai 10
    loopnez a5, 97f
    ee.vld.128.ip q2, a2, 16
    ee.vsr.32 q0, q2
    ee.andq q2, q2, q6
    ee.andq q0, q0, q7
    ee.ldxq.32 q1, q2, a3, 0, 0
    ee.ldxq.32 q1, q2, a3, 1, 1
    ee.ldxq.32 q1, q2, a3, 2, 2
    ee.ldxq.32 q1, q2, a3, 3, 3
    ee.ldxq.32 q3, q2, a3, 0, 4
    ee.ldxq.32 q3, q2, a3, 1, 5
    ee.ldxq.32 q3, q2, a3, 2, 6
    ee.ldxq.32 q3, q2, a3, 3, 7
    ee.vunzip.16 q1, q3
    ee.xorq q1, q1, q0
    ee.srcq.128.st.incp q4, q1, a4
    ee.orq q4, q1, q1
97:
    ee.srcq.128.st.incp q4, q1, a7
    retw
    .size dvbs2_pie_run8_packed, . - dvbs2_pie_run8_packed

/* Former compact-table mapper, retained for radio-off A/B timing. */
    .align 4
    .global dvbs2_pie_run8_compact
    .type dvbs2_pie_run8_compact, @function
dvbs2_pie_run8_compact:
    entry a1, 32
    wur.sar_byte a6
    splat q5, 0x00300030
    splat q6, 0x00030003
    splat q7, 0x000c000c
    ee.zero.q q4
    srli a8, a5, 1
    loopnez a8, 98f
    ee.vld.128.ip q2, a2, 16
    ssai 2
    ee.vsr.32 q0, q2
    ee.andq q0, q0, q6
    ssai 4
    ee.vsr.32 q1, q2
    ee.andq q1, q1, q7
    ee.orq q0, q0, q1
    ssai 6
    ee.vsr.32 q1, q2
    ee.andq q1, q1, q5
    ee.orq q0, q0, q1
    ee.andq q3, q2, q6
    ssai 2
    ee.vsr.32 q1, q2
    ee.andq q1, q1, q7
    ee.orq q3, q3, q1
    ssai 4
    ee.vsr.32 q1, q2
    ee.andq q1, q1, q5
    ee.orq q2, q3, q1
    pair_lookup 0
    ee.srcq.128.st.incp q4, q1, a4
    ee.orq q4, q1, q1
    pair_lookup 4
    ee.srcq.128.st.incp q4, q1, a4
    ee.orq q4, q1, q1
98:
    /* All 8PSK runs are multiples of 32 symbols (1440 or 21600). */
    ee.srcq.128.st.incp q4, q1, a7
    retw
    .size dvbs2_pie_run8_compact, . - dvbs2_pie_run8_compact

/* LDPC parity packing: append each column's q bits (natural order) to a
 * big-endian bit stream of 32-bit words. Column t's 32 rows of block b sit,
 * big-endian, at p + cols[t] + 1536 b. The last q % 32 rows (always some) are
 * the top bits of the last word. Bits wait right-aligned in 'pending'; 'have'
 * of them are valid and SAR = have, so SRC pending:v is the next output word.
 * void dvbs2_pie_pack(const uint8_t *p, const uint16_t *cols, unsigned q,
 *                     uint32_t *out, uint32_t pending, unsigned have)
 * a2 p, a3 cols, a5 out, a6 pending, a7 have, a9 part, a10 32 - part,
 * a11 1536, a12 offset of the last word, a14 column, a15 v, a4/a8/a13 temp. */
    .macro full_word offset
    .if \offset
    add a4, a14, a11
    l32i a15, a4, 0
    .else
    l32i a15, a14, 0
    .endif
    src a4, a6, a15
    s32i a4, a5, 0
    addi a5, a5, 4
    mov a6, a15
    .endm

    .macro last_word
    add a4, a14, a12
    l32i a15, a4, 0
    add a13, a7, a9                     /* have + part */
    bltui a13, 32, 1f
    src a4, a6, a15                     /* a word completes */
    s32i a4, a5, 0
    addi a5, a5, 4
    addi a7, a13, -32
    ssr a10
    srl a6, a15                         /* leftover bits, right-aligned */
    j 2f
1:
    ssr a10
    src a6, a6, a15                     /* pending:v >> (32 - part) */
    mov a7, a13
2:  /* not the loop end: a jump to the end of a zero-overhead loop leaves it */
    ssr a7
    .endm

    .macro column words
    l16ui a14, a3, 0
    addi a3, a3, 2
    add a14, a14, a2
    .if \words >= 1
    full_word 0
    .endif
    .if \words >= 2
    full_word 1
    .endif
    last_word
    .endm

    .align 4
    .global dvbs2_pie_pack
    .type dvbs2_pie_pack, @function
dvbs2_pie_pack:
    entry a1, 32
    srli a8, a4, 5                      /* whole words per column: 0, 1, 2 */
    extui a9, a4, 0, 5
    movi a10, 32
    sub a10, a10, a9
    movi a11, 1536
    mull a12, a8, a11
    movi a13, 360
    ssr a7
    beqz a8, 30f
    bnei a8, 1, 32f
    loop a13, 31f
    column 1
31:
    retw
32:
    loop a13, 33f
    column 2
33:
    retw
30:
    loop a13, 34f
    column 0
34:
    retw
    .size dvbs2_pie_pack, . - dvbs2_pie_pack

/* Byte-reverse 32-bit words and store them at any address: vectors x 16
 * bytes from src (aligned) to dst, given as block = (dst - 1) & ~15 and
 * sar_byte = -dst & 15. Bytes from block to dst are overwritten; the last
 * aligned block goes to tail (16 bytes) for the caller.
 * Reversal per register pair: unzip/zip bytes, then unzip/zip halfwords.
 * void dvbs2_pie_bswap(const uint32_t *src, uint8_t *block, unsigned vectors,
 *                      unsigned sar_byte, uint8_t *tail) */
    .align 4
    .global dvbs2_pie_bswap
    .type dvbs2_pie_bswap, @function
dvbs2_pie_bswap:
    entry a1, 32
    wur.sar_byte a5
    ee.zero.q q0
    srli a8, a4, 1
    loopnez a8, 35f
    ee.vld.128.ip q1, a2, 16
    ee.vld.128.ip q2, a2, 16
    ee.vunzip.8 q1, q2
    ee.vzip.8 q2, q1
    ee.vunzip.16 q2, q1
    ee.vzip.16 q1, q2                   /* q1, q2: words reversed */
    ee.srcq.128.st.incp q0, q1, a3
    ee.srcq.128.st.incp q1, q2, a3
    ee.orq q0, q2, q2
35:
    bbci a4, 0, 36f
    ee.vld.128.ip q1, a2, 16
    ee.zero.q q2
    ee.vunzip.8 q1, q2
    ee.vzip.8 q2, q1
    ee.vunzip.16 q2, q1
    ee.vzip.16 q1, q2
    ee.srcq.128.st.incp q0, q1, a3
    ee.orq q0, q1, q1
36:
    ee.srcq.128.st.incp q0, q1, a6
    retw
    .size dvbs2_pie_bswap, . - dvbs2_pie_bswap

/* TS CRC-8, four bytes per step (scalar; scheduled so no load is used by the next
 * instruction): crc' = T3[crc ^ b0] ^ T2[b1] ^ T1[b2] ^ T0[b3], Tk = tables + 256 k.
 * uint32_t dvbs2_pie_crc8(const uint8_t *p, unsigned quads, const uint8_t *tables,
 *                         uint32_t crc) */
    .align 4
    .global dvbs2_pie_crc8
    .type dvbs2_pie_crc8, @function
dvbs2_pie_crc8:
    entry a1, 32
    addmi a6, a4, 256                   /* T1 */
    addmi a7, a4, 512                   /* T2 */
    addmi a8, a4, 768                   /* T3 */
    loopnez a3, 37f
    l8ui a9, a2, 0
    l8ui a10, a2, 1
    l8ui a11, a2, 2
    l8ui a12, a2, 3
    xor a9, a9, a5
    add a10, a10, a7
    add a11, a11, a6
    add a12, a12, a4
    add a9, a9, a8
    l8ui a10, a10, 0
    l8ui a11, a11, 0
    l8ui a12, a12, 0
    l8ui a9, a9, 0
    xor a10, a10, a11
    addi a2, a2, 4
    xor a10, a10, a12
    xor a5, a9, a10
37:
    mov a2, a5
    retw
    .size dvbs2_pie_crc8, . - dvbs2_pie_crc8

/* Nibble packing for internal-RAM rings: 32 phases (two aligned vectors, values
 * 0..7) to 16 bytes, byte j = s[2j] | s[2j + 1] << 4. VUNZIP.8 splits even and
 * odd symbols (TRM v1.8 1.8.209); the odd ones move up four bits within their
 * byte, which cannot carry into the next byte. dst may equal src.
 * void dvbs2_pie_nibbles(const uint8_t *src, uint8_t *dst, unsigned vectors) */
    .align 4
    .global dvbs2_pie_nibbles
    .type dvbs2_pie_nibbles, @function
dvbs2_pie_nibbles:
    entry a1, 32
    ssai 4
    loopnez a4, 40f
    ee.vld.128.ip q0, a2, 16
    ee.vld.128.ip q1, a2, 16
    ee.vunzip.8 q0, q1                  /* q0 even symbols, q1 odd */
    ee.vsl.32 q1, q1
    ee.orq q0, q0, q1
    ee.vst.128.ip q0, a3, 16
40:
    retw
    .size dvbs2_pie_nibbles, . - dvbs2_pie_nibbles
