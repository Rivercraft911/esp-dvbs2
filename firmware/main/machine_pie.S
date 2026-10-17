// Offsets are 16-bit byte distances within separate compact .20 table parts.
// Consumer has no symbol history, multiply, helper dispatch or join.
// TRM v1.8 1.7.1/Table 1.7-2; fused add/load/store: 1.8.74/75.
    .section .iram1.machine,"ax"

    .macro byte
    l8ui a9,a3,0
    slli a8,a8,8
    addi a3,a3,1
    or a8,a8,a9
    .endm

    .macro rows shift,off
    extui a10,a8,16+\shift,6
    extui a11,a8,8+\shift,8
    extui a12,a8,\shift,8
    mull a10,a10,a14
    mull a11,a11,a14
    mull a12,a12,a14
    s16i a10,a2,\off
    s16i a11,a2,\off+2
    s16i a12,a2,\off+4
    .endm

    .global machine_prepare_pie
    .type machine_prepare_pie,@function
    .align 4
machine_prepare_pie:
    entry a1,32
    beqz a4,.Lprepare_done
    l8ui a8,a3,0
    l8ui a9,a3,1
    slli a8,a8,8
    or a8,a8,a9
    addi a3,a3,2
    movi a14,48
    loopnez a4,.Lprepare_end
    byte
    rows 2,0
    byte
    rows 0,6
    byte
    byte
    rows 6,12
    byte
    rows 4,18
    addi a2,a2,24
.Lprepare_end:
.Lprepare_done:
    retw
    .size machine_prepare_pie,.-machine_prepare_pie

    .global machine_render_pie
    .type machine_render_pie,@function
    .align 4
machine_render_pie:
    entry a1,32
    beqz a4,.Lrender_done
    movi a8,0x80200
    ee.movi.32.q q7,a8,0
    ee.movi.32.q q7,a8,1
    ee.movi.32.q q7,a8,2
    ee.movi.32.q q7,a8,3
    movi a6,3072
    movi a7,15360
    add a6,a6,a5
    add a7,a7,a5
    loopnez a4,.Lrender_end
    l16ui a10,a3,0
    l16ui a11,a3,2
    l16ui a12,a3,4
    add a10,a10,a5
    add a11,a11,a6
    add a12,a12,a7
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
    addi a3,a3,6
.Lrender_end:
.Lrender_done:
    retw
    .size machine_render_pie,.-machine_render_pie
