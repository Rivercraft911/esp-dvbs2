// Experiment only. Twelve samples/block; n3 uses fused adds and memory work.
// Single-issue/load-use model: ESP32-S3 TRM v1.8 1.7.1/Table 1.7-2 pp65-73.
// EE.VADDS.S32.LD.INCP/ST.INCP: 1.8.74/75 pp150-151.
    .section .iram1.shaper_moon,"ax"

    .macro xor_constant
    movi a8,0x80200
    ee.movi.32.q q7,a8,0
    ee.movi.32.q q7,a8,1
    ee.movi.32.q q7,a8,2
    ee.movi.32.q q7,a8,3
    .endm

    .macro three_vectors
    ee.vld.128.ip q0,a10,16
    ee.vld.128.ip q1,a11,16
    ee.vld.128.ip q2,a10,16
    ee.vadds.s32.ld.incp q3,a11,q0,q0,q1
    ee.vld.128.ip q1,a12,16
    ee.vadds.s32.ld.incp q6,a10,q2,q2,q3
    ee.vadds.s32.ld.incp q3,a11,q0,q0,q1
    ee.vld.128.ip q1,a12,16
    ee.xorq q4,q0,q7
    ee.vadds.s32.ld.incp q0,a12,q2,q2,q1
    ee.vadds.s32.st.incp q4,a2,q6,q6,q3
    ee.xorq q5,q2,q7
    ee.vadds.s32.st.incp q5,a2,q6,q6,q0
    ee.xorq q4,q6,q7
    ee.vst.128.ip q4,a2,16
    .endm

    .global shaper_moon3_pie
    .type shaper_moon3_pie,@function
    .align 4
shaper_moon3_pie:
    entry a1,32
    beqz a4,.Lthree_done
    xor_constant
    addi a9,a3,-4
    l16ui a8,a9,0
    l16ui a9,a9,2
    slli a8,a8,12
    or a8,a8,a9
    loopnez a4,.Lthree_end
    l16ui a9,a3,0
    slli a8,a8,12
    or a8,a8,a9
    extui a10,a8,18,9
    extui a11,a8,9,9
    extui a12,a8,0,9
    addx2 a10,a10,a10
    addx2 a11,a11,a11
    addx2 a12,a12,a12
    slli a10,a10,4
    slli a11,a11,4
    slli a12,a12,4
    add a10,a10,a5
    add a11,a11,a6
    add a12,a12,a7
    three_vectors
    addi a3,a3,2
.Lthree_end:
.Lthree_done:
    retw
    .size shaper_moon3_pie,.-shaper_moon3_pie

// Rational kernels use four/five independent rows. Both retain the fractional
// phase by advancing exactly five symbols for every twelve output samples.
// Table base pointers are passed in a four/five-element array.
    .macro row_address R,off
    addx2 \R,\R,\R
    l32i a14,a5,\off
    slli \R,\R,4
    add \R,\R,a14
    .endm

    .macro rational_vectors parts
    // Each vector contains four complex samples. The n3 kernel above has a
    // fully fused schedule; these larger rational sums retain explicit adds.
    .rept 3
    ee.vld.128.ip q0,a9,16
    ee.vld.128.ip q1,a10,16
    ee.vld.128.ip q2,a11,16
    ee.vadds.s32.ld.incp q3,a12,q0,q0,q1
    .if \parts == 5
    ee.vadds.s32.ld.incp q1,a13,q0,q0,q2
    ee.vadds.s32 q0,q0,q3
    ee.vadds.s32 q0,q0,q1
    .else
    ee.vadds.s32 q0,q0,q2
    ee.vadds.s32 q0,q0,q3
    .endif
    ee.xorq q4,q0,q7
    ee.vst.128.ip q4,a2,16
    .endr
    .endm

    .macro rational name,parts,three=0
    .global \name
    .type \name,@function
    .align 4
\name:
    entry a1,32
    beqz a4,.Ldone\@
    xor_constant
    addi a14,a3,-4
    l16ui a6,a14,0
    l16ui a8,a14,2
    .if \three
    slli a6,a6,12
    .elseif \parts == 4
    slli a6,a6,10
    .else
    slli a6,a6,15
    movi a7,0
    .endif
    or a6,a6,a8
    loopnez a4,.Lend\@
    l16ui a8,a3,0
    .if \three
    extui a7,a6,20,4
    slli a6,a6,12
    or a6,a6,a8
    srli a9,a6,27
    slli a7,a7,5
    or a9,a9,a7
    extui a10,a6,18,9
    extui a11,a6,9,9
    extui a12,a6,0,9
    .elseif \parts == 4
    slli a6,a6,10
    or a6,a6,a8
    extui a9,a6,18,8
    extui a10,a6,10,8
    extui a11,a6,2,8
    extui a12,a6,0,2
    .else
    srli a14,a6,17
    slli a7,a7,15
    or a7,a7,a14
    slli a6,a6,15
    or a6,a6,a8
    srli a9,a6,30
    extui a14,a7,0,7
    addx4 a9,a14,a9
    extui a10,a6,21,9
    extui a11,a6,12,9
    extui a12,a6,3,9
    extui a13,a6,0,3
    .endif
    row_address a9,0
    row_address a10,4
    row_address a11,8
    row_address a12,12
    .if \parts == 5
    row_address a13,16
    .endif
    rational_vectors \parts
    addi a3,a3,2
.Lend\@:
.Ldone\@:
    retw
    .size \name,.-\name
    .endm
    rational shaper_moon24q_pie,4
    rational shaper_moon24p_pie,5

    rational shaper_moon3a20_pie,4,1
