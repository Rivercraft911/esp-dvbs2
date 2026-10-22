/* Independent native preflight: seed/snapshot all PIE state around one actual
 * leaf call. Data layout224B: QR128,QACC64,ACCX16,SAR/SAR_BYTE/CPE/PS16.
 * QACC/ACCX padding is untouched; caller zeroes snapshots. No C call executes
 * inside the seed/snapshot interval. Original caller state is restored too. */
#include "rx_halfband_context.inc"
.macro snapshot ptr
    ee.vst.128.ip q0,\ptr,16
    ee.vst.128.ip q1,\ptr,16
    ee.vst.128.ip q2,\ptr,16
    ee.vst.128.ip q3,\ptr,16
    ee.vst.128.ip q4,\ptr,16
    ee.vst.128.ip q5,\ptr,16
    ee.vst.128.ip q6,\ptr,16
    ee.vst.128.ip q7,\ptr,16
    ee.st.qacc_h.l.128.ip \ptr,16
    ee.st.qacc_h.h.32.ip \ptr,16
    ee.st.qacc_l.l.128.ip \ptr,16
    ee.st.qacc_l.h.32.ip \ptr,16
    ee.st.accx.ip \ptr,16
    rsr a8,sar
    s32i a8,\ptr,0
    rur.sar_byte a8
    s32i a8,\ptr,4
    rsr a8,cpenable
    s32i a8,\ptr,8
    rsr a8,ps
    s32i a8,\ptr,12
.endm
.section .iram1,"ax",@progbits
.align 4
.global hb_context_call
.type hb_context_call,@function
hb_context_call:
    entry a1,320
    s32i a2,a1,256
    s32i a3,a1,260
    s32i a4,a1,264
    s32i a5,a1,268
    s32i a6,a1,272
    pie_enter
    l32i a12,a1,264
    ee.vld.128.ip q0,a12,16
    ee.vld.128.ip q1,a12,16
    ee.vld.128.ip q2,a12,16
    ee.vld.128.ip q3,a12,16
    ee.vld.128.ip q4,a12,16
    ee.vld.128.ip q5,a12,16
    ee.vld.128.ip q6,a12,16
    ee.vld.128.ip q7,a12,16
    ee.ld.qacc_h.l.128.ip a12,16
    ee.ld.qacc_h.h.32.ip a12,16
    ee.ld.qacc_l.l.128.ip a12,16
    ee.ld.qacc_l.h.32.ip a12,16
    ee.ld.accx.ip a12,16
    l32i a8,a12,0
    extui a8,a8,0,6
    wsr a8,sar
    l32i a8,a12,4
    extui a8,a8,0,4
    wur.sar_byte a8
    l32i a12,a1,268
    snapshot a12
    l32i a8,a1,256
    l32i a9,a1,260
    l32i a10,a9,0
    l32i a11,a9,4
    l32i a12,a9,8
    l32i a13,a9,12
    l32i a14,a9,16
    l32i a15,a9,20
    callx8 a8
    l32i a12,a1,272
    snapshot a12
    pie_leave
.size hb_context_call,.-hb_context_call

/* Official Xtensa HAL routine exercises real window overflow/underflow without
 * touching PIE or scalar SAR. Used only by the native preflight flag. */
.align 4
.global hb_forced_window_spill
.type hb_forced_window_spill,@function
hb_forced_window_spill:
    entry a1,32
    call8 xthal_window_spill
    retw.n
.size hb_forced_window_spill,.-hb_forced_window_spill
