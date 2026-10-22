/* Independent eight-check NMS leaf. All operands/constants are internal.
 * q[10][8] is updated in place. Two minima, last argmin and ten outgoing
 * signs form the exact four-byte/check state. No expanded next plane.
 * Entry a2 points to the 240-byte rx_mc6_tile in mc6_internal.h. */
#ifndef RX_MC6_SCALAR
.section .iram1,"ax",@progbits
.balign 4
.global rx_mc6_leaf
.type rx_mc6_leaf,@function
rx_mc6_leaf:
    entry a1,32
    rsil a13,15
    rsr.cpenable a8
    movi a9,8
    or a9,a9,a8
    wsr.cpenable a9
    rsync
    rsr.sar a12
    addi a3,a2,112
    l32r a6,.Lmc6_constants_ptr
    ee.vld.128.ip q3,a6,0
    mv.qr q4,q3
    ee.zero.q q5
    ee.zero.q q6
    addi a4,a2,16
    movi a10,10
.Lmc6_min:
    ee.vld.128.ip q0,a4,16
    ee.vsubs.s16 q1,q6,q0
    ee.vmax.s16 q2,q0,q1
    ee.vmax.s16 q7,q3,q2
    ee.vmin.s16 q4,q4,q7
    ee.vmin.s16 q3,q3,q2
    ee.vcmp.lt.s16 q7,q0,q6
    ee.xorq q5,q5,q7
    addi a10,a10,-1
    bnez a10,.Lmc6_min

    /* q0 raw minimum, q1 packed normalized minima, q2 sign parity,
     * q3 new metadata, q4 zero; q5/q6/q7 are update temporaries. */
    mv.qr q0,q3
    movi a9,2
    wsr.sar a9
    addi a6,a6,16
    ee.vld.128.ip q7,a6,16
    ee.vmul.u16 q3,q3,q7
    ee.vmul.u16 q4,q4,q7
    ee.vld.128.ip q7,a6,16
    ee.vmin.s16 q3,q3,q7
    ee.vmin.s16 q4,q4,q7
    addi a5,a3,96
    ee.vst.128.ip q4,a5,0
    movi a9,8
    wsr.sar a9
    ee.vsl.32 q4,q4
    ee.orq q1,q3,q4
    addi a5,a3,64
    ee.vst.128.ip q1,a5,0
    mv.qr q2,q5
    ee.zero.q q3
    ee.zero.q q4
    addi a4,a2,16
    l32r a7,.Lmc6_edges_ptr
    movi a10,10
.Lmc6_update:
    ee.vld.128.ip q5,a4,0
    ee.vsubs.s16 q7,q4,q5
    ee.vmax.s16 q7,q7,q5
    ee.vcmp.eq.s16 q7,q7,q0
    /* Replace only argmin bits for every equal minimum: LAST tie wins. */
    ee.vld.128.ip q6,a6,0
    ee.andq q5,q7,q6
    ee.notq q5,q5
    ee.andq q3,q3,q5
    ee.vld.128.ip q5,a7,16
    ee.andq q5,q5,q7
    ee.orq q3,q3,q5
    addi a5,a3,96
    ee.vld.128.ip q6,a5,0
    ee.andq q6,q6,q7
    ee.notq q7,q7
    ee.andq q7,q7,q1
    ee.orq q6,q6,q7
    addi a5,a6,16
    ee.vld.128.ip q7,a5,0
    ee.andq q6,q6,q7
    ee.vld.128.ip q5,a4,0
    ee.vcmp.lt.s16 q7,q5,q4
    ee.xorq q7,q7,q2
    ee.xorq q6,q6,q7
    ee.vsubs.s16 q6,q6,q7
    ee.vadds.s16 q5,q5,q6
    ee.vst.128.ip q5,a4,16
    ee.vld.128.ip q6,a7,16
    ee.andq q7,q7,q6
    ee.orq q3,q3,q7
    addi a10,a10,-1
    bnez a10,.Lmc6_update
    addi a5,a3,80
    ee.vst.128.ip q3,a5,0
    wsr.sar a12
    wsr.cpenable a8
    rsync
    wsr.ps a13
    rsync
    retw
.size rx_mc6_leaf,.-rx_mc6_leaf

.literal .Lmc6_constants_ptr,.Lmc6_constants
.literal .Lmc6_edges_ptr,.Lmc6_edges
.section .dram1,"aw",@progbits
.balign 16
.Lmc6_constants:
.rept 8
.short 32767
.endr
.rept 8
.short 3
.endr
.rept 8
.short 127
.endr
.rept 8
.short 0x3c00
.endr
.rept 8
.short 255
.endr
.Lmc6_edges:
.set edge,0
.rept 10
.rept 8
.short edge*1024
.endr
.rept 8
.short 1<<edge
.endr
.set edge,edge+1
.endr
#endif
