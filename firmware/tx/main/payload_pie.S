// Synthetic payload: eight xorshift32 lanes (shifts 13, 17, 5) at once. Lanes 0-3 live in q0,
// lanes 4-7 in q1. Each step advances every lane once and keeps its low byte; four steps
// make one little-endian word per lane, stored at dst + j*stride + 4u. Lane j therefore
// writes the contiguous block [j*stride, (j+1)*stride) when it starts at M^(j*stride) x.
// EE.VSR.32 is arithmetic (TRM v1.8 1.8.207): mask after the 17-bit right shift, and after
// the 8-bit shift of the byte accumulators. 27 vector/SAR instructions per step.
// void payload_xorshift8(uint8_t *dst, unsigned quads, uint32_t lanes[8], unsigned stride)
// dst and stride word aligned, quads >= 1, lanes 16-byte aligned and updated.
    .macro xs_step
    ssai 13
    ee.vsl.32 q6,q0
    ee.vsl.32 q7,q1
    ee.xorq q0,q0,q6
    ee.xorq q1,q1,q7
    ssai 17
    ee.vsr.32 q6,q0
    ee.vsr.32 q7,q1
    ee.andq q6,q6,q4
    ee.andq q7,q7,q4
    ee.xorq q0,q0,q6
    ee.xorq q1,q1,q7
    ssai 5
    ee.vsl.32 q6,q0
    ee.vsl.32 q7,q1
    ee.xorq q0,q0,q6
    ee.xorq q1,q1,q7
    ssai 8
    ee.vsr.32 q2,q2
    ee.vsr.32 q3,q3
    ee.andq q2,q2,q5
    ee.andq q3,q3,q5
    ssai 24
    ee.vsl.32 q6,q0
    ee.vsl.32 q7,q1
    ee.orq q2,q2,q6
    ee.orq q3,q3,q7
    .endm

    .macro xs_store q,lane,ptr
    ee.movi.32.a \q,a6,\lane
    s32i a6,\ptr,0
    addi \ptr,\ptr,4
    .endm

    .section .iram1.payload,"ax"
    .align 4
    .global payload_xorshift8
    .type payload_xorshift8,@function
payload_xorshift8:
    entry a1,32
    ee.vld.128.ip q0,a4,16
    ee.vld.128.ip q1,a4,-16
    movi a6,0x7fff
    ee.movi.32.q q4,a6,0
    ee.movi.32.q q4,a6,1
    ee.movi.32.q q4,a6,2
    ee.movi.32.q q4,a6,3
    movi a6,0x00ffffff
    ee.movi.32.q q5,a6,0
    ee.movi.32.q q5,a6,1
    ee.movi.32.q q5,a6,2
    ee.movi.32.q q5,a6,3
    mov a7,a2
    add a8,a7,a5
    add a9,a8,a5
    add a10,a9,a5
    add a11,a10,a5
    add a12,a11,a5
    add a13,a12,a5
    add a14,a13,a5
.Lxs_loop:
    xs_step
    xs_step
    xs_step
    xs_step
    xs_store q2,0,a7
    xs_store q2,1,a8
    xs_store q2,2,a9
    xs_store q2,3,a10
    xs_store q3,0,a11
    xs_store q3,1,a12
    xs_store q3,2,a13
    xs_store q3,3,a14
    addi a3,a3,-1
    bnez a3,.Lxs_loop
    ee.vst.128.ip q0,a4,16
    ee.vst.128.ip q1,a4,0
    retw
    .size payload_xorshift8,.-payload_xorshift8
