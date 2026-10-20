/* Independent raw32 -> sc8 packing from the S3 TRM shift/unzip semantics.
 * Eight input words become one output vector. No loads outside the input. */
.section .iram1, "ax", @progbits
.align 4
.global rx_pack8
.type rx_pack8, @function
rx_pack8:
    entry a1, 32
    beqz a4, .Lreturn
    rsr a8, cpenable
    movi a9, 8
    or a9, a9, a8
    wsr a9, cpenable
    rsync
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
    wsr a8, cpenable
    rsync
.Lreturn:
    retw.n
.size rx_pack8, .-rx_pack8

/* Independently written vector prescan and clear, TRM1.8 EQ.S32/MOVI.32.A. */
.macro enable
    rsr a8, cpenable
    movi a9, 8
    or a9, a9, a8
    wsr a9, cpenable
    rsync
.endm
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
    entry a1, 32
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
    wsr a8, cpenable
    rsync
    retw.n
.size rx_pack_scan8, .-rx_pack_scan8

.align 4
.global rx_pack_clear4
.type rx_pack_clear4, @function
rx_pack_clear4:
    entry a1, 32
    beqz a3, .Lclear_return
    enable
    sentinel
    loopnez a3, .Lcleared
    ee.vst.128.ip q4, a2, 16
.Lcleared:
    memw
    wsr a8, cpenable
    rsync
.Lclear_return:
    retw.n
.size rx_pack_clear4, .-rx_pack_clear4
