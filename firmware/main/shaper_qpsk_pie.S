// Four symbols/block; three rows, n vectors/row. 9-symbol pulse, no sample packing.
// TRM v1.8 1.7.1 pp65-66 / Table 1.7-2 pp68-69: load-use interlocks;
// 1.8.74-75 pp150-151: fused add/load and add/store; 1.8.88 p164: aligned load.
    .section .iram1.shaper_qpsk,"ax"

    .macro address reg, off, n
    l8ui \reg,a3,\off
    .if \n == 3
    addx2 \reg,\reg,\reg
    slli \reg,\reg,4
    .else
    slli \reg,\reg,6
    .endif
    add \reg,\reg,a5
    .endm

    .macro kernel name,n
    .global \name
    .type \name,@function
    .align 4
\name:
    entry a1,32
    beqz a4,.Ldone\@
    movi a8,0x80200
    ee.movi.32.q q7,a8,0
    ee.movi.32.q q7,a8,1
    ee.movi.32.q q7,a8,2
    ee.movi.32.q q7,a8,3
    // Previous block's consumed part-0/part-1 pointers. Advance them each iteration:
    // p2 <- p1 + (part bytes - row bytes); p1 <- p0 + that stride; p0 <- new row.
    addi a3,a3,-2
    address a11,0,\n
    address a10,1,\n
    movi a6,(256*16*\n + 16*\n)
    add a11,a11,a6
    addi a10,a10,(16*\n)
    movi a6,(256*16*\n - 16*\n)
    addi a3,a3,2
    loopnez a4,.Lend\@
    l8ui a8,a3,0
    add a12,a11,a6
    add a11,a10,a6
    .if \n == 3
    addx2 a8,a8,a8
    slli a8,a8,4
    .else
    slli a8,a8,6
    .endif
    add a10,a8,a5

    // Three vectors: 9 loads + 3 stores + 3 XORs, all six additions fused.
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
    .if \n == 4
    // The last vector can use the preceding vector's final add/XOR slots for loads.
    ee.vld.128.ip q0,a10,16
    ee.xorq q4,q6,q7
    ee.vld.128.ip q1,a11,16
    ee.vld.128.ip q2,a12,16
    ee.vadds.s32.st.incp q4,a2,q0,q0,q1
    ee.vadds.s32 q0,q0,q2
    ee.xorq q4,q0,q7
    .else
    ee.xorq q4,q6,q7
    .endif
    ee.vst.128.ip q4,a2,16
    addi a3,a3,1
.Lend\@:
.Ldone\@:
    retw
    .size \name,.-\name
    .endm
    kernel shaper_qpsk3_pie,3
    kernel shaper_qpsk4_pie,4

// Copy sixteen chronological quad bytes, with a 0/2/4/6-bit left shift in each
// byte and the following byte supplying its low bits. Unaligned source, aligned
// destination, sixteen readable source guards. TRM v1.8 1.8.17 / 1.8.49.
    .global ring_copy_quads_pie
    .type ring_copy_quads_pie,@function
    .align 4
ring_copy_quads_pie:
    entry a1,32
    beqz a4,.Lcopy_done
    movi a6,8
    sub a6,a6,a5
    movi a8,255
    ssl a5
    sll a8,a8
    extui a8,a8,0,8
    movi a9,0x01010101
    mull a8,a8,a9
    ee.movi.32.q q7,a8,0
    ee.movi.32.q q7,a8,1
    ee.movi.32.q q7,a8,2
    ee.movi.32.q q7,a8,3
    movi a9,-1
    xor a8,a8,a9
    ee.movi.32.q q6,a8,0
    ee.movi.32.q q6,a8,1
    ee.movi.32.q q6,a8,2
    ee.movi.32.q q6,a8,3
    loopnez a4,.Lcopy_end
    ee.ld.128.usar.ip q0,a3,16
    ee.vld.128.ip q1,a3,0
    ee.src.q q0,q0,q1
    addi a8,a3,-15
    ee.ld.128.usar.ip q1,a8,16
    ee.vld.128.ip q2,a8,0
    ee.src.q q1,q1,q2
    ssr a5
    ee.vsl.32 q0,q0
    ssr a6
    ee.vsr.32 q1,q1
    ee.andq q0,q0,q7
    ee.andq q1,q1,q6
    ee.orq q0,q0,q1
    ee.vst.128.ip q0,a2,16
.Lcopy_end:
.Lcopy_done:
    retw
    .size ring_copy_quads_pie,.-ring_copy_quads_pie
