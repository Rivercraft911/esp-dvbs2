// Six cached window mappings rotate through all three parts, as in shaper_pie.S.
// Physical parts are [2,0,1], so every row address uses a signed 2048-byte offset.
// The loop issues 32 instructions/symbol plus one pointer increment per six;
// Phase loads are interleaved with vector loads. Counts exclude SRAM stalls.
    .macro rows reg, off, qi, qq, phaseoff=-1
    l32i a9,\reg,0
    l32i a8,\reg,4
    .if \off
    addmi a9,a9,\off
    addmi a8,a8,\off
    .endif
    ee.vld.128.ip \qi,a9,0
    .if \phaseoff >= 0
    l8ui a9,a3,\phaseoff
    .endif
    ee.vld.128.ip \qq,a8,0
    .endm

    .macro symbol R,P,off
    rows \R,-2048,q0,q1,\off
    srli a9,a9,4
    addx8 a6,a6,a9
    extui a8,a6,0,9
    addx8 \R,a8,a7
    rows \P,2048,q2,q3
    l32i a9,\R,0
    l32i a8,\R,4
    ee.vadds.s32.ld.incp q2,a9,q0,q0,q2
    ee.vadds.s32.ld.incp q3,a8,q1,q1,q3
    ssai 6
    ee.vadds.s32 q0,q0,q2
    ee.vadds.s32 q1,q1,q3
    // I sums <=1023, so a six-bit shift stays within each 16-bit lane.
    ee.vsl.32 q0,q0
    ee.vzip.16 q0,q1
    ee.vsr.32 q0,q0
    ee.vsr.32 q1,q1
    ee.xorq q0,q0,q7
    ee.xorq q1,q1,q7
    ee.vst.128.ip q0,a2,16
    ee.vst.128.ip q1,a2,16
    .endm

    .section .vcm, "ax"
    .global shaper_axes8_pie
    .type shaper_axes8_pie,@function
    .align 4
// groups >=0 produces 6*groups+1 symbols. Eight history bytes, scaled phases 16*k.
shaper_axes8_pie:
    entry a1,32
    movi a8,0x80200
    ee.movi.32.q q7,a8,0
    ee.movi.32.q q7,a8,1
    ee.movi.32.q q7,a8,2
    ee.movi.32.q q7,a8,3
    movi a8,6144
    add a7,a5,a8
    movi a6,0
    addi a9,a3,-8
    .rept 2
    l8ui a8,a9,0
    srli a8,a8,4
    addx8 a6,a6,a8
    addi a9,a9,1
    .endr
    .macro window reg
    l8ui a8,a9,0
    srli a8,a8,4
    addx8 a6,a6,a8
    extui a8,a6,0,9
    addx8 \reg,a8,a7
    addi a9,a9,1
    .endm
    window a15
    window a10
    window a11
    window a12
    window a13
    window a14
    symbol a15,a12,0
    addi a3,a3,1
    loopnez a4,.Laxes_end
    symbol a10,a13,0
    symbol a11,a14,1
    symbol a12,a15,2
    symbol a13,a10,3
    symbol a14,a11,4
    symbol a15,a12,5
    addi a3,a3,6
.Laxes_end:
    retw
    .size shaper_axes8_pie,.-shaper_axes8_pie
