/* Private collector: complete PIE ownership for one bounded leaf, TRM1.8.
 * Frame288: locals through244, followed by at least32B reserved for call8 spill.
 * No calls or external-memory operands occur while interrupts are masked.
 * Only core1 owns the counters. elapsed includes entry/exit and any interrupt
 * immediately after restoration, so it conservatively bounds protected time.
 */
.macro pie_enter
    rsr a14,ccount
    rsil a8,15
    s32i a8,a1,224
    s32i a14,a1,240
    rsr a8,cpenable
    s32i a8,a1,228
    rsr a9,sar
    s32i a9,a1,232
    movi a9,8
    or a9,a9,a8
    wsr a9,cpenable
    rsync
    rur.sar_byte a9
    s32i a9,a1,236
    addi a10,a1,16
    ee.vst.128.ip q0,a10,16
    ee.vst.128.ip q1,a10,16
    ee.vst.128.ip q2,a10,16
    ee.vst.128.ip q3,a10,16
    ee.vst.128.ip q4,a10,16
    ee.vst.128.ip q5,a10,16
    ee.vst.128.ip q6,a10,16
    ee.vst.128.ip q7,a10,16
    ee.st.qacc_h.l.128.ip a10,16
    ee.st.qacc_h.h.32.ip a10,16
    ee.st.qacc_l.l.128.ip a10,16
    ee.st.qacc_l.h.32.ip a10,16
    ee.st.accx.ip a10,0
.endm
.macro pie_leave
    movi a10,hb_force_spill
    l32i a9,a10,0
    beqz a9,.Lpie_no_spill\@
    call8 hb_forced_window_spill
    movi a10,hb_forced_spill_calls
    l32i a9,a10,0
    addi a9,a9,1
    s32i a9,a10,0
.Lpie_no_spill\@:
    movi a10,144
    add a10,a1,a10
    ee.ld.qacc_h.l.128.ip a10,16
    ee.ld.qacc_h.h.32.ip a10,16
    ee.ld.qacc_l.l.128.ip a10,16
    ee.ld.qacc_l.h.32.ip a10,16
    ee.ld.accx.ip a10,0
    addi a10,a1,16
    ee.vld.128.ip q0,a10,16
    ee.vld.128.ip q1,a10,16
    ee.vld.128.ip q2,a10,16
    ee.vld.128.ip q3,a10,16
    ee.vld.128.ip q4,a10,16
    ee.vld.128.ip q5,a10,16
    ee.vld.128.ip q6,a10,16
    ee.vld.128.ip q7,a10,16
    l32i a9,a1,236
    wur.sar_byte a9
    l32i a9,a1,232
    wsr a9,sar
    l32i a8,a1,228
    wsr a8,cpenable
    rsync
    l32i a8,a1,224
    wsr a8,ps
    rsync
    rsr a8,ccount
    l32i a9,a1,240
    sub a8,a8,a9
    movi a10,hb_leaf_max_cycles
    l32i a9,a10,0
    bgeu a9,a8,.Lpie_count\@
    s32i a8,a10,0
.Lpie_count\@:
    movi a10,hb_leaf_calls
    l32i a9,a10,0
    addi a9,a9,1
    s32i a9,a10,0
    retw.n
.endm
