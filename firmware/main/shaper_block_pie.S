// Packed 12-bit groups remove phase unpacking and sample packing. Three table
// vectors sum to each aligned output vector. TRM v1.8 1.8.74-75 pp150-151.
    .section .iram1.shaper_block,"ax"
    .global shaper_block_pie
    .type shaper_block_pie,@function
    .align 4
shaper_block_pie:
    entry a1,32
    beqz a4,.Lblock_done
    movi a9,0x80200
    ee.movi.32.q q7,a9,0
    ee.movi.32.q q7,a9,1
    ee.movi.32.q q7,a9,2
    ee.movi.32.q q7,a9,3
    addi a9,a3,-4
    l16ui a8,a9,0
    l16ui a9,a9,2
    slli a8,a8,12
    or a8,a8,a9
    .macro block_address
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
    .endm
    .macro block_vector first=0
    ee.vld.128.ip q0,a10,16
    ee.vld.128.ip q1,a11,16
    ee.vld.128.ip q2,a12,16
    .if \first
    ee.vadds.s32 q0,q0,q1
    .else
    ee.vadds.s32.st.incp q4,a2,q0,q0,q1
    .endif
    ee.vadds.s32 q0,q0,q2
    ee.xorq q4,q0,q7
    .endm
    block_address
    block_vector 1
    block_vector
    block_vector
    addi a3,a3,2
    addi a4,a4,-1
    loopnez a4,.Lblock_end
    block_address
    block_vector
    block_vector
    block_vector
    addi a3,a3,2
.Lblock_end:
    ee.vst.128.ip q4,a2,16
.Lblock_done:
    retw
    .size shaper_block_pie,.-shaper_block_pie
