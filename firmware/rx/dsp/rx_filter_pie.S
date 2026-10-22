/* Independent ESP32-S3 TRM1.8 §§1.8.19,59,146,209 implementation.
 * All code/literals are IRAM. Dot inputs/tables/sums are internal; staging
 * may read a cache-enabled SC8 source. No calls while IRQs are masked.
 * QR0..5 are private caller-clobbered. No scalar SAR, QACC or FCR writes. */
.section .iram1,"ax",@progbits
.balign 4
.macro component pointer,result
    ee.zero.accx
    mov a15,\pointer
    beqz a11,.Laligned\@
    movi a9,-16
    and a15,a15,a9
    ee.vld.128.ip q0,a15,16
    ee.vld.128.ip q1,a15,16
    ee.vld.128.ip q2,a15,0
    ee.src.q q3,q0,q1
    ee.vmulas.s8.accx q3,q4
    ee.src.q q3,q1,q2
    ee.vmulas.s8.accx q3,q5
    j .Lsum\@
.Laligned\@:
    ee.vld.128.ip q0,a15,16
    ee.vld.128.ip q1,a15,0
    ee.vmulas.s8.accx q0,q4
    ee.vmulas.s8.accx q1,q5
.Lsum\@:
    rur.accx_0 \result
.endm

.global rx_filter_dot_s8
.type rx_filter_dot_s8,@function
rx_filter_dot_s8:
    entry a1,64
    rsil a6,5
    rsr a7,cpenable
    movi a8,8
    or a8,a8,a7
    wsr a8,cpenable
    rsync
    rsr a9,cpenable
    bne a9,a8,.Ldot_enable_failed
    rur.sar_byte a10
    addi a14,a1,16
    ee.st.accx.ip a14,0
    extui a11,a2,0,4
    wur.sar_byte a11
    ee.vld.128.ip q4,a4,16
    ee.vld.128.ip q5,a4,0
    component a2,a12
    component a3,a13
    s32i a12,a5,0
    s32i a13,a5,4
    ee.ld.accx.ip a14,0
    wur.sar_byte a10
    movi a2,1
    rur.sar_byte a9
    bne a9,a10,.Ldot_bad
    rur.accx_0 a9
    l32i a11,a1,16
    bne a9,a11,.Ldot_bad
    rur.accx_1 a9
    l32i a11,a1,20
    extui a9,a9,0,8
    extui a11,a11,0,8
    beq a9,a11,.Ldot_restore
.Ldot_bad:
    movi a2,0
    j .Ldot_restore
.Ldot_enable_failed:
    movi a2,0
.Ldot_restore:
    wsr a7,cpenable
    rsync
    rsr a9,cpenable
    beq a9,a7,.Ldot_unmask
    movi a2,0
.Ldot_unmask:
    wsr a6,ps
    rsync
    retw.n
.size rx_filter_dot_s8,.-rx_filter_dot_s8

.balign 4
.global rx_filter_unpack_s8
.type rx_filter_unpack_s8,@function
rx_filter_unpack_s8:
    entry a1,32
    rsil a6,5
    rsr a7,cpenable
    movi a8,8
    or a8,a8,a7
    wsr a8,cpenable
    rsync
    rsr a9,cpenable
    bne a9,a8,.Lunpack_failed
    beqz a5,.Lunpack_done
    loopnez a5,.Lunpack_end
    ee.vld.128.ip q0,a2,16
    ee.vld.128.ip q1,a2,16
    ee.vunzip.8 q0,q1
    ee.vst.128.ip q0,a3,16
    ee.vst.128.ip q1,a4,16
.Lunpack_end:
.Lunpack_done:
    movi a2,1
    j .Lunpack_restore
.Lunpack_failed:
    movi a2,0
.Lunpack_restore:
    wsr a7,cpenable
    rsync
    rsr a9,cpenable
    beq a9,a7,.Lunpack_unmask
    movi a2,0
.Lunpack_unmask:
    wsr a6,ps
    rsync
    retw.n
.size rx_filter_unpack_s8,.-rx_filter_unpack_s8
