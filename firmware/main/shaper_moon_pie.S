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

    .macro three name,step,first,width0,shift1,width1
    .global \name
    .type \name,@function
    .align 4
\name:
    entry a1,32
    beqz a4,.Lthree_done\@
    xor_constant
    addi a9,a3,-4
    l16ui a8,a9,0
    l16ui a9,a9,2
    slli a8,a8,\step
    or a8,a8,a9
    movi a14,48
    loopnez a4,.Lthree_end\@
    l16ui a9,a3,0
    slli a8,a8,\step
    or a8,a8,a9
    extui a10,a8,\first,\width0
    extui a11,a8,\shift1,\width1
    extui a12,a8,0,\width1
    mull a10,a10,a14
    mull a11,a11,a14
    mull a12,a12,a14
    add a10,a10,a5
    add a11,a11,a6
    add a12,a12,a7
    three_vectors
    addi a3,a3,2
.Lthree_end\@:
.Lthree_done\@:
    retw
    .size \name,.-\name

    .endm
    three shaper_moon3_pie,12,18,9,9,9
    three shaper_moon24qc_pie,10,16,6,8,8

// Compact QPSK directly from the ring's high-pair-first quad bytes. Four
// blocks consume five new bytes. Keep the preceding symbols in a8 instead
// of writing and reloading a temporary uint16_t stream. src starts at the
// first of eleven contributing symbols, on a quad boundary; count is in
// four-block (48-sample) iterations. Same three table rows as the block API.
    .macro quad_rows shift
    extui a10,a8,16+\shift,6
    extui a11,a8,8+\shift,8
    extui a12,a8,\shift,8
    mull a10,a10,a14
    mull a11,a11,a14
    mull a12,a12,a14
    add a10,a10,a5
    add a11,a11,a6
    add a12,a12,a7
    three_vectors
    .endm

    .macro quad_byte
    l8ui a9,a3,0
    slli a8,a8,8
    addi a3,a3,1
    or a8,a8,a9
    .endm

    .global shaper_moon24qc_quads_pie
    .type shaper_moon24qc_quads_pie,@function
    .align 4
shaper_moon24qc_quads_pie:
    entry a1,32
    beqz a4,.Lquad_done
    xor_constant
    l32i a7,a5,8
    l32i a6,a5,4
    l32i a5,a5,0
    l8ui a8,a3,0
    l8ui a9,a3,1
    slli a8,a8,8
    or a8,a8,a9
    addi a3,a3,2
    movi a14,48
    loopnez a4,.Lquad_end
    quad_byte
    quad_rows 2
    quad_byte
    quad_rows 0
    quad_byte
    quad_byte
    quad_rows 6
    quad_byte
    quad_rows 4
.Lquad_end:
.Lquad_done:
    retw
    .size shaper_moon24qc_quads_pie,.-shaper_moon24qc_quads_pie

// QPSK's two five-symbol rows are reused as whole windows. The oldest
// symbol only affects the first vector; its row occupies sixteen bytes.
    .global shaper_moon24q_pie
    .type shaper_moon24q_pie,@function
    .align 4
shaper_moon24q_pie:
    entry a1,32
    beqz a4,.Lq_done
    xor_constant
    l32i a7,a5,8
    l32i a6,a5,4
    l32i a5,a5,0
    movi a14,48
    sub a15,a6,a7
    addi a15,a15,-48
    addi a3,a3,-4
    l16ui a8,a3,2
    movi a9,48
    mull a12,a8,a9
    add a12,a12,a7
    addi a12,a12,48
    loopnez a4,.Lq_end
    l16ui a8,a3,0
    l16ui a9,a3,4
    extui a8,a8,0,2
    add a11,a12,a15
    slli a10,a8,4
    mull a12,a9,a14
    add a10,a10,a5
    add a12,a12,a7
    ee.vld.128.ip q0,a11,16
    ee.vld.128.ip q1,a12,16
    ee.vld.128.ip q2,a11,16
    ee.vadds.s32.ld.incp q3,a12,q0,q0,q1
    ee.vld.128.ip q1,a10,16
    ee.vadds.s32.ld.incp q4,a11,q2,q2,q3
    ee.vadds.s32.ld.incp q5,a12,q0,q0,q1
    ee.xorq q6,q0,q7
    ee.vadds.s32.st.incp q6,a2,q4,q4,q5
    ee.xorq q6,q2,q7
    ee.vst.128.ip q6,a2,16
    ee.xorq q6,q4,q7
    ee.vst.128.ip q6,a2,16
    addi a3,a3,2
.Lq_end:
.Lq_done:
    retw
    .size shaper_moon24q_pie,.-shaper_moon24q_pie

// Seven-symbol 8PSK: four independent rows, twelve loads, three stores and
// three XORs. All nine additions fuse with memory operations.
    .global shaper_moon24p_pie
    .type shaper_moon24p_pie,@function
    .align 4
shaper_moon24p_pie:
    entry a1,32
    beqz a4,.Lp_done
    xor_constant
    mov a15,a5
    l32i a5,a15,0
    l32i a6,a15,4
    l32i a7,a15,8
    l32i a8,a15,12
    addi a9,a3,-4
    l16ui a14,a9,0
    l16ui a10,a9,2
    slli a14,a14,15
    or a14,a14,a10
    movi a15,48
    loopnez a4,.Lp_end
    extui a9,a14,9,9
    l16ui a10,a3,0
    slli a14,a14,15
    or a14,a14,a10
    extui a10,a14,15,9
    extui a11,a14,6,9
    extui a12,a14,0,6
    mull a9,a9,a15
    mull a10,a10,a15
    mull a11,a11,a15
    mull a12,a12,a15
    add a9,a9,a5
    add a10,a10,a6
    add a11,a11,a7
    add a12,a12,a8
    ee.vld.128.ip q0,a9,16
    ee.vld.128.ip q1,a10,16
    ee.vld.128.ip q2,a9,16
    ee.vadds.s32.ld.incp q3,a10,q0,q0,q1
    ee.vld.128.ip q1,a11,16
    ee.vadds.s32.ld.incp q4,a12,q2,q2,q3
    ee.vadds.s32.ld.incp q3,a11,q0,q0,q1
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
    ee.vst.128.ip q4,a2,16
    addi a3,a3,2
.Lp_end:
.Lp_done:
    retw
    .size shaper_moon24p_pie,.-shaper_moon24p_pie
