// TRM v1.8 1.7, Table 1.7-2: single issue, load-use interlocks.
// Queue words contain scaled offsets. No bank preparation or multiplication.
    .macro lofty_word offset,advance
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
    ee.vld.128.xp q1,a12,a15
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
    .endm
    .section .iram1.lofty,"ax"
    .global lofty_render_pie
    .type lofty_render_pie,@function
    .align 4
lofty_render_pie:
    entry a1,32
    beqz a4,.Ldone
    movi a8,0x80200
    ee.movi.32.q q7,a8,0
    ee.movi.32.q q7,a8,1
    ee.movi.32.q q7,a8,2
    ee.movi.32.q q7,a8,3
    movi a6,10240
    add a6,a6,a5
    movi a7,34816
    add a7,a7,a5
    addmi a13,a5,8192
    movi a15,1024
    l32i a8,a3,0
    // Two words keep the hardware loop short and leave IRAM for the mux.
    extui a14,a4,0,1
    srli a4,a4,1
    loopnez a4,.Lend
    lofty_word 4,0
    lofty_word 0,8
.Lend:
    loopnez a14,.Ltail
    lofty_word 0,4
.Ltail:
.Ldone:
    retw
    .size lofty_render_pie,.-lofty_render_pie

// Eight four-symbol words -> eight offsets. Output aligned; source has 16 guard bytes.
    .section .iram1.lofty_prepare,"ax"
    .global lofty_prepare_pie
    .type lofty_prepare_pie,@function
    .align 4
lofty_prepare_pie:
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
    .size lofty_prepare_pie,.-lofty_prepare_pie
