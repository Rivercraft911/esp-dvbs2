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
    movi a14,48
    loopnez a4,.Lthree_end
    l16ui a9,a3,0
    slli a8,a8,12
    or a8,a8,a9
    extui a10,a8,18,9
    extui a11,a8,9,9
    extui a12,a8,0,9
    mull a10,a10,a14
    mull a11,a11,a14
    mull a12,a12,a14
    add a10,a10,a5
    add a11,a11,a6
    add a12,a12,a7
    three_vectors
    addi a3,a3,2
.Lthree_end:
.Lthree_done:
    retw
    .size shaper_moon3_pie,.-shaper_moon3_pie

// All additions fuse with loads/stores: 4 rows need 18 vector instructions,
// 5 rows need 21. Bases and the 48-byte stride stay in registers throughout.
    .macro rational_vectors parts
    ee.vld.128.ip q0,a9,16
    ee.vld.128.ip q1,a10,16
    ee.vld.128.ip q2,a9,16
    ee.vadds.s32.ld.incp q3,a10,q0,q0,q1
    ee.vld.128.ip q1,a11,16
    ee.vadds.s32.ld.incp q4,a12,q2,q2,q3
    ee.vadds.s32.ld.incp q3,a11,q0,q0,q1
    .if \parts == 4
    ee.vld.128.ip q1,a9,16
    ee.vadds.s32.ld.incp q5,a12,q0,q0,q4
    ee.vadds.s32.ld.incp q6,a10,q2,q2,q3
    ee.xorq q4,q0,q7
    ee.vadds.s32.ld.incp q0,a11,q2,q2,q5
    ee.vadds.s32.ld.incp q3,a12,q1,q1,q6
    ee.xorq q5,q2,q7
    ee.vadds.s32.st.incp q4,a2,q1,q1,q0
    ee.vadds.s32.st.incp q5,a2,q1,q1,q3
    ee.xorq q4,q1,q7
    .else
    ee.vld.128.ip q1,a13,16
    ee.vadds.s32.ld.incp q4,a9,q0,q0,q4
    ee.vadds.s32.ld.incp q5,a10,q2,q2,q3
    ee.vadds.s32.ld.incp q1,a12,q0,q0,q1
    ee.vadds.s32.ld.incp q6,a13,q4,q4,q5
    ee.xorq q5,q0,q7
    ee.vadds.s32.ld.incp q0,a11,q2,q2,q1
    ee.vadds.s32.ld.incp q1,a12,q2,q2,q6
    ee.xorq q6,q2,q7
    ee.vadds.s32.ld.incp q2,a13,q4,q4,q0
    ee.vadds.s32.st.incp q5,a2,q4,q4,q1
    ee.vadds.s32.st.incp q6,a2,q4,q4,q2
    ee.xorq q4,q4,q7
    .endif
    ee.vst.128.ip q4,a2,16
    .endm

    .macro rational name,parts
    .global \name
    .type \name,@function
    .align 4
\name:
    entry a1,32
    beqz a4,.Ldone\@
    xor_constant
    mov a15,a5
    mov a13,a4
    l32i a4,a15,0
    l32i a5,a15,4
    l32i a6,a15,8
    l32i a7,a15,12
    .if \parts == 5
    l32i a8,a15,16
    .endif
    addi a9,a3,-4
    l16ui a14,a9,0
    l16ui a10,a9,2
    .if \parts == 4
    slli a14,a14,10
    .else
    slli a14,a14,15
    .endif
    or a14,a14,a10
    movi a15,48
    loopnez a13,.Lend\@
    .if \parts == 5
    // Oldest window is already in the preceding 30-bit history. Extract it
    // before shifting, avoiding a 64-bit history and cross-word extraction.
    extui a9,a14,15,9
    .endif
    l16ui a10,a3,0
    .if \parts == 4
    slli a14,a14,10
    or a14,a14,a10
    extui a9,a14,18,8
    extui a10,a14,10,8
    extui a11,a14,2,8
    extui a12,a14,0,2
    .else
    slli a14,a14,15
    or a14,a14,a10
    extui a10,a14,21,9
    extui a11,a14,12,9
    extui a12,a14,3,9
    extui a13,a14,0,3
    .endif
    mull a9,a9,a15
    mull a10,a10,a15
    mull a11,a11,a15
    mull a12,a12,a15
    .if \parts == 5
    mull a13,a13,a15
    .endif
    add a9,a9,a4
    add a10,a10,a5
    add a11,a11,a6
    add a12,a12,a7
    .if \parts == 5
    add a13,a13,a8
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
