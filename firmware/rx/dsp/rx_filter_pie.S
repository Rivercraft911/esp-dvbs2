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

/* a2/a3 planes, a4 bank, a5 requests, a6 count, a7 internal sums.
 * Each four-byte request is {uint16 offset, uint16 phase}. No calls,
 * literals or external-memory accesses occur inside this <=8-output mask. */
.balign 4
.global rx_filter_batch_s8
.type rx_filter_batch_s8,@function
rx_filter_batch_s8:
    entry a1,96
    beqz a6,.Lbatch_refuse
    movi a8,8
    bltu a8,a6,.Lbatch_refuse
    s32i a2,a1,24
    s32i a3,a1,28
    s32i a4,a1,32
    s32i a5,a1,36
    s32i a6,a1,40
    s32i a7,a1,44
    rsil a8,5
    s32i a8,a1,0
    rsr a9,cpenable
    s32i a9,a1,4
    movi a8,8
    or a8,a8,a9
    wsr a8,cpenable
    rsync
    rsr a9,cpenable
    bne a9,a8,.Lbatch_enable_failed
    rur.sar_byte a10
    s32i a10,a1,8
    addi a14,a1,16
    ee.st.accx.ip a14,0
.Lbatch_next:
    l32i a8,a1,36
    l16ui a5,a8,0
    l16ui a4,a8,2
    slli a4,a4,5
    l32i a9,a1,32
    add a4,a9,a4
    l32i a2,a1,24
    add a2,a2,a5
    l32i a3,a1,28
    add a3,a3,a5
    extui a11,a2,0,4
    wur.sar_byte a11
    ee.vld.128.ip q4,a4,16
    ee.vld.128.ip q5,a4,0
    component a2,a12
    component a3,a13
    l32i a7,a1,44
    s32i a12,a7,0
    s32i a13,a7,4
    addi a7,a7,8
    s32i a7,a1,44
    l32i a8,a1,36
    addi a8,a8,4
    s32i a8,a1,36
    l32i a6,a1,40
    addi a6,a6,-1
    s32i a6,a1,40
    bnez a6,.Lbatch_next
    ee.ld.accx.ip a14,0
    l32i a10,a1,8
    wur.sar_byte a10
    movi a2,1
    rur.sar_byte a9
    bne a9,a10,.Lbatch_bad
    rur.accx_0 a9
    l32i a11,a1,16
    bne a9,a11,.Lbatch_bad
    rur.accx_1 a9
    l32i a11,a1,20
    extui a9,a9,0,8
    extui a11,a11,0,8
    beq a9,a11,.Lbatch_restore
.Lbatch_bad:
    movi a2,0
    j .Lbatch_restore
.Lbatch_enable_failed:
    movi a2,0
.Lbatch_restore:
    l32i a7,a1,4
    wsr a7,cpenable
    rsync
    rsr a9,cpenable
    beq a9,a7,.Lbatch_unmask
    movi a2,0
.Lbatch_unmask:
    l32i a6,a1,0
    wsr a6,ps
    rsync
    retw.n
.Lbatch_refuse:
    movi a2,0
    retw.n
.size rx_filter_batch_s8,.-rx_filter_batch_s8
