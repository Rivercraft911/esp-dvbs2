/* QPSK only. Convert chronological quad bytes into words containing sixteen
 * I signs (15:0) and Q signs (31:16), oldest first in each halfword. */
    .section .iram1.qpsk_signs,"ax"
    .macro splat reg,value
    movi a8,\value
    ee.movi.32.q \reg,a8,0
    ee.movi.32.q \reg,a8,1
    ee.movi.32.q \reg,a8,2
    ee.movi.32.q \reg,a8,3
    .endm
    .macro unshuffle shift,mask
    ssai \shift
    ee.vsr.32 q1,q0
    ee.xorq q1,q1,q0
    ee.andq q1,q1,\mask
    ee.vsl.32 q2,q1
    ee.xorq q0,q0,q1
    ee.xorq q0,q0,q2
    .endm
    .align 4
    .global dvbs2_qpsk_planes
    .type dvbs2_qpsk_planes,@function
dvbs2_qpsk_planes:
    entry a1,32
    splat q3,0x55555555
    splat q4,0x22222222
    splat q5,0x0c0c0c0c
    splat q6,0x00f000f0
    splat q7,0x0000ff00
    mov a4,a2
    loopnez a3,.Lplanes_end
    ee.vld.128.ip q0,a2,16
    ssai 1
    ee.vsr.32 q1,q0
    ee.andq q1,q1,q3
    ee.xorq q0,q0,q1
    ee.zero.q q1
    ee.vunzip.8 q0,q1
    ee.vzip.8 q1,q0
    ee.vunzip.16 q1,q0
    ee.vzip.16 q0,q1
    unshuffle 1,q4
    unshuffle 2,q5
    unshuffle 4,q6
    unshuffle 8,q7
    ee.vst.128.ip q0,a4,16
.Lplanes_end:
    retw
    .size dvbs2_qpsk_planes,.-dvbs2_qpsk_planes

/* Sixteen histories / eighty symbols. Source and output need only word
 * alignment. Carries the six prior signs through five plane words. */
    .section .iram1.qpsk_histories,"ax"
    .align 4
    .global dvbs2_qpsk_histories
    .type dvbs2_qpsk_histories,@function
dvbs2_qpsk_histories:
    entry a1,32
    extui a6,a5,4,11
    extui a7,a5,20,11
    movi a14,0x7ff07ff0
    loopnez a4,.Lhistories_end
    l32i a8,a3,0
    slli a9,a6,16
    extui a6,a8,0,16
    slli a10,a7,16
    srli a7,a8,16
    or a9,a9,a6
    or a10,a10,a7
    extui a11,a9,11,11
    extui a12,a10,11,11
    slli a11,a11,4
    slli a12,a12,20
    or a11,a11,a12
    s32i a11,a2,0
    extui a11,a9,6,11
    extui a12,a10,6,11
    slli a11,a11,4
    slli a12,a12,20
    or a11,a11,a12
    s32i a11,a2,4
    slli a11,a8,3
    and a11,a11,a14
    s32i a11,a2,8
    l32i a8,a3,4
    slli a9,a6,16
    extui a6,a8,0,16
    slli a10,a7,16
    srli a7,a8,16
    or a9,a9,a6
    or a10,a10,a7
    extui a11,a9,12,11
    extui a12,a10,12,11
    slli a11,a11,4
    slli a12,a12,20
    or a11,a11,a12
    s32i a11,a2,12
    extui a11,a9,7,11
    extui a12,a10,7,11
    slli a11,a11,4
    slli a12,a12,20
    or a11,a11,a12
    s32i a11,a2,16
    slli a11,a8,2
    and a11,a11,a14
    s32i a11,a2,20
    l32i a8,a3,8
    slli a9,a6,16
    extui a6,a8,0,16
    slli a10,a7,16
    srli a7,a8,16
    or a9,a9,a6
    or a10,a10,a7
    extui a11,a9,13,11
    extui a12,a10,13,11
    slli a11,a11,4
    slli a12,a12,20
    or a11,a11,a12
    s32i a11,a2,24
    extui a11,a9,8,11
    extui a12,a10,8,11
    slli a11,a11,4
    slli a12,a12,20
    or a11,a11,a12
    s32i a11,a2,28
    slli a11,a8,1
    and a11,a11,a14
    s32i a11,a2,32
    l32i a8,a3,12
    slli a9,a6,16
    extui a6,a8,0,16
    slli a10,a7,16
    srli a7,a8,16
    or a9,a9,a6
    or a10,a10,a7
    extui a11,a9,14,11
    extui a12,a10,14,11
    slli a11,a11,4
    slli a12,a12,20
    or a11,a11,a12
    s32i a11,a2,36
    extui a11,a9,9,11
    extui a12,a10,9,11
    slli a11,a11,4
    slli a12,a12,20
    or a11,a11,a12
    s32i a11,a2,40
    mov a11,a8
    and a11,a11,a14
    s32i a11,a2,44
    l32i a8,a3,16
    slli a9,a6,16
    extui a6,a8,0,16
    slli a10,a7,16
    srli a7,a8,16
    or a9,a9,a6
    or a10,a10,a7
    extui a11,a9,15,11
    extui a12,a10,15,11
    slli a11,a11,4
    slli a12,a12,20
    or a11,a11,a12
    s32i a11,a2,48
    extui a11,a9,10,11
    extui a12,a10,10,11
    slli a11,a11,4
    slli a12,a12,20
    or a11,a11,a12
    s32i a11,a2,52
    srli a11,a8,1
    and a11,a11,a14
    s32i a11,a2,56
    slli a11,a8,4
    and a11,a11,a14
    s32i a11,a2,60
    addi a3,a3,20
    addi a2,a2,64
.Lhistories_end:
    mov a2,a11
    retw
    .size dvbs2_qpsk_histories,.-dvbs2_qpsk_histories

/* Shift a run of sixteen-sign plane words by 1..15 symbols. The extra
 * source word is inside the same PL span. dst/src only need word alignment. */
    .section .iram1.qpsk_signs,"ax"
    .align 4
    .global dvbs2_qpsk_shift
    .type dvbs2_qpsk_shift,@function
dvbs2_qpsk_shift:
    entry a1,32
    movi a6,1
    ssl a5
    sll a6,a6
    addi a6,a6,-1
    slli a7,a6,16
    or a6,a6,a7
    movi a7,16
    sub a7,a7,a5
    l32i a8,a3,0
    loopnez a4,.Lshift_end
    ssl a5
    sll a9,a8
    l32i a8,a3,4
    ssr a7
    srl a10,a8
    xor a10,a10,a9
    and a10,a10,a6
    xor a9,a9,a10
    s32i a9,a2,0
    addi a3,a3,4
    addi a2,a2,4
.Lshift_end:
    retw
    .size dvbs2_qpsk_shift,.-dvbs2_qpsk_shift
