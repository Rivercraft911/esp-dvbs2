#ifndef LOFTY_FUSED
#define LOFTY_FUSED 0
#endif
// TRM v1.8 1.7, Table 1.7-2: single issue, load-use interlocks.
// Default queue words contain scaled offsets; fused words reorder phase histories.
    .macro lofty_word offset,advance
#if LOFTY_FUSED
    extui a10,a8,15,13
    extui a11,a8,12,13
    extui a12,a8,9,13
    extui a9,a8,0,13
    add a10,a10,a5
    add a11,a11,a6
    add a12,a12,a7
    add a9,a9,a13
    ee.vld.128.ip q0,a10,0
    ee.vld.128.xp q1,a9,a14
    ee.vld.128.ip q2,a11,0
    ee.vld.128.xp q3,a9,a14
    ee.vadds.s32.ld.incp q5,a9,q0,q0,q1
    ee.vld.128.ip q6,a12,0
    ee.xorq q4,q0,q7
    ee.vadds.s32.st.incp q4,a2,q2,q2,q3
    ee.xorq q4,q2,q7
    ee.vadds.s32.st.incp q4,a2,q6,q6,q5
    .if \advance
    addi a3,a3,\advance
    .endif
    l32i a8,a3,\offset
    ee.xorq q4,q6,q7
    ee.vst.128.ip q4,a2,16
#else
    extui a10,a8,0,13
    extui a11,a8,9,15
    extui a12,a8,20,10
    add a10,a10,a5
    add a11,a11,a6
    add a12,a12,a7
    ee.vld.128.ip q0,a10,0
    extui a9,a8,0,10
    ee.vld.128.ip q1,a11,16
    addx2 a9,a9,a13
    ee.vadds.s32.ld.incp q2,a9,q0,q0,q1
    ee.vld.128.ip q3,a11,16
    ee.vld.128.ip q5,a9,0
    ee.vld.128.ip q1,a12,1024
    ee.vadds.s32.ld.incp q3,a11,q6,q2,q3
    ee.xorq q4,q0,q7
    ee.vadds.s32.ld.incp q0,a12,q6,q6,q1
    ee.vadds.s32.st.incp q4,a2,q5,q5,q3
    ee.xorq q4,q6,q7
    ee.vadds.s32.st.incp q4,a2,q5,q5,q0
    .if \advance
    addi a3,a3,\advance
    .endif
    l32i a8,a3,\offset
    ee.xorq q4,q5,q7
    ee.vst.128.ip q4,a2,16
#endif
    .endm
#if LOFTY_FUSED
    // Locked with the mux only during 26m667; costs no permanent internal RAM.
    .section .lofty_fused,"ax"
#else
    .section .iram1.lofty,"ax"
#endif
    .global lofty_render_pie
    .global lofty_render_pages_pie
    .type lofty_render_pie,@function
    .type lofty_render_pages_pie,@function
    .align 4
#if !LOFTY_FUSED
    // Preserve the qualified fetch position without raising IRAM alignment.
    // The linker assertion below the stream mappings guards this placement.
    // 16: the short-frame options moved the IRAM code before this section 8 bytes lower.
    .space 16
#endif
lofty_render_pages_pie:
    entry a1,32
    mov a15,a6
    s32i a4,a1,0
    beqz a4,.Ldone
    movi a4,128
    j .Lsetup
lofty_render_pie:
    entry a1,32
    movi a15,0
    beqz a4,.Ldone
.Lsetup:
    movi a8,0x80200
    ee.movi.32.q q7,a8,0
    ee.movi.32.q q7,a8,1
    ee.movi.32.q q7,a8,2
    ee.movi.32.q q7,a8,3
#if LOFTY_FUSED
    movi a6,8192
    add a6,a6,a5
    movi a7,16384
    add a7,a7,a5
    movi a13,24576
    add a13,a13,a5
#else
    movi a6,10240
    add a6,a6,a5
    movi a7,34816
    add a7,a7,a5
    addmi a13,a5,8192
#endif
    l32i a8,a3,0
.Lpage:
    // Two words keep the hardware loop short and leave IRAM for the mux.
    extui a14,a4,0,1
#if LOFTY_FUSED
    s32i a14,a1,4
    movi a14,8192
#endif
    srli a4,a4,1
    loopnez a4,.Lend
    lofty_word 4,0
    lofty_word 0,8
.Lend:
#if LOFTY_FUSED
    l32i a14,a1,4
    beqz a14,.Ltail
    movi a14,8192
    lofty_word 0,4
#else
    loopnez a14,.Ltail
    lofty_word 0,4
#endif
.Ltail:
    beqz a15,.Ldone
    l32i a9,a15,0
    addi a9,a9,1
    memw
    s32i a9,a15,0
    l32i a9,a1,0
    addi a9,a9,-1
    beqz a9,.Ldone
    s32i a9,a1,0
    // Skip the guard and three metadata words. The next source is a page.
    addi a3,a3,16
    l32i a8,a3,0
    movi a4,128
    j .Lpage
.Ldone:
    retw
    .size lofty_render_pie,.-lofty_render_pie
    .size lofty_render_pages_pie,.-lofty_render_pages_pie

#if !LOFTY_FUSED
    // Main 7391f86's cache indices; linker aligns this padding, not .iram1.
    // ldgen sorts .loft0 before the .lofty code mapping.
    .section .loft0,"ax"
    .space 0x720
#endif
// Eight four-symbol words -> eight offsets. Output aligned; source has 16 guard bytes.
    .section .text.lofty_prepare,"ax"
    .global lofty_prepare_pie
    .type lofty_prepare_pie,@function
    .align 4
lofty_prepare_pie:
#if LOFTY_FUSED
    entry a1,32
    movi a8,0x00070007
    ee.movi.32.q q5,a8,0
    ee.movi.32.q q5,a8,1
    ee.movi.32.q q5,a8,2
    ee.movi.32.q q5,a8,3
    movi a8,0x03f003f0
    ee.movi.32.q q6,a8,0
    ee.movi.32.q q6,a8,1
    ee.movi.32.q q6,a8,2
    ee.movi.32.q q6,a8,3
    ee.zero.q q4
    slli a5,a5,16
    ee.movi.32.q q4,a5,3
    movi a8,14
    wur.sar_byte a8
    loopnez a4,.Lfused_prepare_end
    ee.ld.128.usar.ip q0,a3,16
    ssai 2
    ee.vld.128.ip q1,a3,0
    ee.src.q q0,q0,q1
    wur.sar_byte a8
    ee.src.q.qup q1,q4,q0
    ssai 3
    ee.vsr.32 q2,q0
    ee.andq q2,q2,q5
    ee.andq q3,q1,q5
    ee.xorq q1,q1,q3
    ee.orq q1,q1,q2
    ssai 10
    ee.vsl.32 q3,q3
    ssai 2
    ee.vsr.32 q2,q0
    ee.andq q2,q2,q6
    ee.orq q2,q2,q3
    ee.andq q0,q0,q5
    ssai 13
    ee.vsl.32 q0,q0
    ee.orq q2,q2,q0
    ee.vzip.16 q2,q1
    ee.vst.128.ip q2,a2,16
    ee.vst.128.ip q1,a2,16
.Lfused_prepare_end:
    addi a3,a3,-2
    l16ui a2,a3,0
    retw
#else
    entry a1,32
    movi a8,0x01c001c0
    ee.movi.32.q q5,a8,0
    ee.movi.32.q q5,a8,1
    ee.movi.32.q q5,a8,2
    ee.movi.32.q q5,a8,3
    movi a8,0xff80ff80
    ee.movi.32.q q6,a8,0
    ee.movi.32.q q6,a8,1
    ee.movi.32.q q6,a8,2
    ee.movi.32.q q6,a8,3
    movi a8,0x003f003f
    ee.movi.32.q q7,a8,0
    ee.movi.32.q q7,a8,1
    ee.movi.32.q q7,a8,2
    ee.movi.32.q q7,a8,3
    ee.zero.q q4
    slli a5,a5,16
    ee.movi.32.q q4,a5,3
    movi a8,14
    wur.sar_byte a8
    loopnez a4,.Lprepare_end
    ee.ld.128.usar.ip q0,a3,16
    ssai 2
    ee.vld.128.ip q1,a3,0
    ee.src.q q0,q0,q1
    wur.sar_byte a8
    ee.src.q.qup q1,q4,q0
    ssai 4
    ee.vsl.32 q2,q1
    ee.andq q2,q2,q6
    ssai 6
    ee.vsr.32 q3,q0
    ee.andq q3,q3,q7
    ee.vsl.32 q1,q1
    ee.andq q1,q1,q5
    ee.orq q3,q3,q1
    ee.vadds.s32 q1,q3,q3
    ee.vadds.s32 q3,q3,q1
    ee.vzip.16 q2,q3
    ssai 3
    ee.vsr.32 q2,q2
    ee.vsr.32 q3,q3
    ee.andq q0,q0,q7
    ee.zero.q q1
    ee.vzip.16 q0,q1
    ssai 24
    ee.vsl.32 q0,q0
    ee.vsl.32 q1,q1
    ee.orq q2,q2,q0
    ee.orq q3,q3,q1
    ee.vst.128.ip q2,a2,16
    ee.vst.128.ip q3,a2,16
.Lprepare_end:
    addi a3,a3,-2
    l16ui a2,a3,0
    retw
#endif
    .size lofty_prepare_pie,.-lofty_prepare_pie
