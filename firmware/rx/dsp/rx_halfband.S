/* Independent TRM1.8: signed shifts, SRC.Q, VSMULAS.S16, QACC extraction. */
.section .dram1, "aw", @progbits
.balign 16
.Lcoeff:
    .short 274,-1812,9730,16384,4,0,0,0
.Lbias:
    .short 16384,16384,16384,16384,16384,16384,16384,16384

.section .iram1, "ax", @progbits
.align 4
#include "rx_halfband_context.inc"
.macro enable
    pie_enter
.endm
.macro finish
    pie_leave
.endm
.macro unpack
    wsr a6, sar
    ee.vsl.32 q2, q0
    ee.vsl.32 q3, q1
    wsr a7, sar
    ee.vsr.32 q2, q2
    ee.vsr.32 q3, q3
    ee.vsl.32 q0, q0
    ee.vsl.32 q1, q1
    wsr a9, sar
    ee.vsr.32 q0, q0
    ee.vsr.32 q1, q1
    ee.andq q2, q2, q7
    ee.andq q3, q3, q7
    ee.orq q0, q0, q2
    ee.orq q1, q1, q3
    ee.vunzip.32 q0, q1
    ee.vst.128.ip q0, a3, 16
    ee.vst.128.ip q1, a4, 16
.endm
.global hb_unpack8
.type hb_unpack8,@function
hb_unpack8:
    entry a1,288
    beqz a5,.Lunpack_return
    enable
    movi a6,12
    movi a7,22
    movi a9,6
    movi a10,65535
    ee.movi.32.q q7,a10,0
    ee.movi.32.q q7,a10,1
    ee.movi.32.q q7,a10,2
    ee.movi.32.q q7,a10,3
    extui a10,a2,0,4
    beqz a10,.Lunpack_aligned
    wur.sar_byte a10
    movi a10,-16
    and a2,a2,a10
    loopnez a5,.Lunpack_unaligned_done
    ee.vld.128.ip q0,a2,16
    ee.vld.128.ip q1,a2,16
    ee.vld.128.ip q4,a2,0
    ee.src.q q0,q0,q1
    ee.src.q q1,q1,q4
    unpack
.Lunpack_unaligned_done:
    finish
.Lunpack_aligned:
    loopnez a5,.Lunpack_aligned_done
    ee.vld.128.ip q0,a2,16
    ee.vld.128.ip q1,a2,16
    unpack
.Lunpack_aligned_done:
    finish
.Lunpack_return:
    retw.n
.size hb_unpack8,.-hb_unpack8

.macro fir name,shift
.align 4
.global \name
.type \name,@function
\name:
    entry a1,288
    .if \shift==15
    mov a13,a6
    .else
    mov a13,a5
    .endif
    beqz a13,.Lfir_return\@
    enable
    movi a12,.Lcoeff
    ee.vld.128.ip q7,a12,16
    ee.vld.128.ip q6,a12,0
    movi a6,\shift
    movi a7,4
    movi a10,8
    movi a11,12
.if \shift==17
    movi a9,0
.endif
    loopnez a13,.Lfir_done\@
.if \shift==15
    ee.mov.s16.qacc q6
.else
    ee.zero.qacc
    ee.vsmulas.s16.qacc q6,q7,4
.endif
    ee.vld.128.ip q0,a2,16
    ee.vld.128.ip q1,a2,16
    wur.sar_byte a7
    ee.src.q q2,q0,q1
    ee.vsmulas.s16.qacc.ld.incp q3,a2,q0,q7,0
    ee.vsmulas.s16.qacc.ld.incp q5,a3,q2,q7,1
    ee.src.q q4,q1,q3
    ee.vsmulas.s16.qacc.ld.incp q3,a3,q1,q7,1
    ee.vsmulas.s16.qacc q4,q7,0
    wur.sar_byte a10
    ee.src.q q2,q0,q1
    ee.src.q q4,q5,q3
    ee.vsmulas.s16.qacc q4,q7,3
    wur.sar_byte a11
    ee.src.q q3,q0,q1
    ee.vsmulas.s16.qacc q2,q7,2
    ee.vsmulas.s16.qacc q3,q7,2
    ee.srcmb.s16.qacc q0,a6,0
.if \shift==15
    ee.vunzip.32 q0,q1
    ee.vst.l.64.ip q0,a4,8
    ee.vst.l.64.ip q1,a5,8
.else
    /* Shifted40-bit lanes become [signed value20,sign20]. Keep the even bytes. */
    ee.srcmb.s8.qacc q0,a9,0
    ee.vunzip.8 q0,q1
    ee.vst.l.64.ip q0,a4,8
.endif
    addi a2,a2,-32
    addi a3,a3,-16
.Lfir_done\@:
    finish
.Lfir_return\@:
    retw.n
.size \name,.-\name
.endm
fir hb_fir4_15,15
fir hb_fir4_17,17

/* Independent raw32 -> sc8 packing from the S3 TRM shift/unzip semantics.
 * Eight input words become one output vector. No loads outside the input. */

.section .iram1, "ax", @progbits
.align 4
.global rx_pack8
.type rx_pack8, @function
rx_pack8:
    entry a1, 288
    beqz a4, .Lreturn
    pie_enter
    movi a7, 255
    ee.movi.32.q q6, a7, 0
    ee.movi.32.q q6, a7, 1
    ee.movi.32.q q6, a7, 2
    ee.movi.32.q q6, a7, 3
    slli a7, a7, 8
    ee.movi.32.q q7, a7, 0
    ee.movi.32.q q7, a7, 1
    ee.movi.32.q q7, a7, 2
    ee.movi.32.q q7, a7, 3
    movi a5, 12
    movi a6, 6
    loopnez a4, .Lpacked
    ee.vld.128.ip q0, a2, 16
    ee.vld.128.ip q1, a2, 16
    wsr a5, sar
    ee.vsr.32 q2, q0
    ee.vsr.32 q3, q1
    ee.andq q2, q2, q6
    ee.andq q3, q3, q6
    wsr a6, sar
    ee.vsl.32 q0, q0
    ee.vsl.32 q1, q1
    ee.andq q0, q0, q7
    ee.andq q1, q1, q7
    ee.orq q0, q0, q2
    ee.orq q1, q1, q3
    ee.vunzip.16 q0, q1
    ee.vst.128.ip q0, a3, 16
.Lpacked:
    pie_leave
.Lreturn:
    retw.n
.size rx_pack8, .-rx_pack8

/* Independently written vector prescan and clear, TRM1.8 EQ.S32/MOVI.32.A. */
.macro sentinel
    movi a7, 165
    slli a9, a7, 8
    or a7, a7, a9
    slli a9, a7, 16
    or a7, a7, a9
    ee.movi.32.q q4, a7, 0
    ee.movi.32.q q4, a7, 1
    ee.movi.32.q q4, a7, 2
    ee.movi.32.q q4, a7, 3
.endm
.align 4
.global rx_pack_scan8
.type rx_pack_scan8, @function
rx_pack_scan8:
    entry a1, 288
    bnez a3, .Lscan
    movi a2, 1
    retw.n
.Lscan:
    enable
    sentinel
    ee.zero.q q5
    loopnez a3, .Lscanned
    ee.vld.128.ip q0, a2, 16
    ee.vld.128.ip q1, a2, 16
    ee.vcmp.eq.s32 q2, q0, q4
    ee.vcmp.eq.s32 q3, q1, q4
    ee.orq q2, q2, q3
    ee.orq q5, q5, q2
.Lscanned:
    ee.movi.32.a q5, a2, 0
    ee.movi.32.a q5, a3, 1
    ee.movi.32.a q5, a4, 2
    ee.movi.32.a q5, a5, 3
    or a2, a2, a3
    or a4, a4, a5
    or a2, a2, a4
    beqz a2, .Lscan_good
    movi a2, 0
    j .Lscan_done
.Lscan_good:
    movi a2, 1
.Lscan_done:
    pie_leave
.size rx_pack_scan8, .-rx_pack_scan8

.align 4
.global rx_pack_clear4
.type rx_pack_clear4, @function
rx_pack_clear4:
    entry a1, 288
    beqz a3, .Lclear_return
    enable
    sentinel
    loopnez a3, .Lcleared
    ee.vst.128.ip q4, a2, 16
.Lcleared:
    memw
    pie_leave
.Lclear_return:
    retw.n
.size rx_pack_clear4, .-rx_pack_clear4
