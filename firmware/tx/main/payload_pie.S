// Synthetic payload: eight xorshift32 lanes (shifts 13, 17, 5), four at a time in one vector.
// Lane j writes the contiguous block [j*stride, (j+1)*stride) when it starts at M^(j*stride) x.
// Four steps keep their states S1..S4 in q1..q4; six byte unzips (EE.VUNZIP.8: evens to the
// first register, odds to the second, TRM v1.8 1.8.209) then gather each lane's four low bytes,
// oldest first, into the low word of q1, q2, q5 or q6. EE.VSR.32 is arithmetic (1.8.191): mask
// after the 17-bit right shift. 59 instructions per 16 bytes (four 10-instruction steps, a
// copy, six unzips, four moves, four stores, four pointer steps) instead of 67.
// void payload_xorshift8(uint8_t *dst, unsigned quads, uint32_t lanes[8], unsigned stride)
// dst and stride word aligned, quads >= 1, lanes 16-byte aligned and updated.
    .macro xs_step x, y
    ssai 13
    ee.vsl.32 q6,\x
    ee.xorq q5,\x,q6
    ssai 17
    ee.vsr.32 q6,q5
    ee.andq q6,q6,q7
    ee.xorq q5,q5,q6
    ssai 5
    ee.vsl.32 q6,q5
    ee.xorq \y,q5,q6
    .endm

    // Four lanes from q4 (state) at a7, a8, a9, a10 = dst + 0..3 stride; a3 quads.
    .macro xs_pass
    loopnez a3,1f
    xs_step q4,q1
    xs_step q1,q2
    xs_step q2,q3
    xs_step q3,q4
    ee.orq q0,q4,q4
    ee.vunzip.8 q1,q2                   // S1 and S2 even bytes
    ee.vunzip.8 q3,q0                   // S3 and S4 even bytes
    ee.vunzip.8 q1,q3                   // low bytes, step-major: S1 l0..l3, S2 .., S3 .., S4 ..
    ee.vunzip.8 q1,q2                   // lanes 0, 2 | 1, 3
    ee.vunzip.8 q1,q5                   // lane 0 | lane 2
    ee.vunzip.8 q2,q6                   // lane 1 | lane 3
    ee.movi.32.a q1,a11,0
    s32i a11,a7,0
    ee.movi.32.a q2,a11,0
    s32i a11,a8,0
    ee.movi.32.a q5,a11,0
    s32i a11,a9,0
    ee.movi.32.a q6,a11,0
    s32i a11,a10,0
    addi a7,a7,4
    addi a8,a8,4
    addi a9,a9,4
    addi a10,a10,4
1:
    .endm

    .section .iram1.payload,"ax"
    .align 4
    .global payload_xorshift8
    .type payload_xorshift8,@function
payload_xorshift8:
    entry a1,32
    movi a6,0x7fff
    ee.movi.32.q q7,a6,0
    ee.movi.32.q q7,a6,1
    ee.movi.32.q q7,a6,2
    ee.movi.32.q q7,a6,3
    mov a7,a2
    add a8,a7,a5
    add a9,a8,a5
    add a10,a9,a5
    ee.vld.128.ip q4,a4,0
    xs_pass
    ee.vst.128.ip q4,a4,16
    slli a6,a3,2                        // lanes 4..7 start four blocks on
    sub a7,a10,a6
    add a7,a7,a5
    add a8,a7,a5
    add a9,a8,a5
    add a10,a9,a5
    ee.vld.128.ip q4,a4,0
    xs_pass
    ee.vst.128.ip q4,a4,0
    retw
    .size payload_xorshift8,.-payload_xorshift8
