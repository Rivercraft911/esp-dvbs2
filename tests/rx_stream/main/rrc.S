.section .dram1,"aw",@progbits
.balign 16
.global rrc4_symbol_coeff
rrc4_symbol_coeff:
.Ltap:
    .short 51,-7,-64,-62,4,69,61,-21
    .short -96,-74,45,151,123,-39,-190,-168
    .short 33,218,157,-156,-417,-242,420,1071
    .short 936,-362,-2215,-3091,-1388,3389,9958,15682

/* Independently authored from TRM1.8 §§1.8.15/84/88/207/209/210/212.
 * Byte signmask + ZIP widens SC8 without scaling. Swap adjacent16bit lanes,
 * then FFT word-reverse store gives the existing reversed S16 planes exactly.
 * Caller owns q/SAR_BYTE; no live vector state on return. CPE restored. */
.section .iram1,"ax",@progbits
.balign 4
.macro planes
    ee.vunzip.8 q0,q1
    ee.vcmp.lt.s8 q2,q0,q4
    ee.vcmp.lt.s8 q3,q1,q4
    ee.vzip.8 q0,q2
    ee.vzip.8 q1,q3
    ee.vst.128.ip q0,a3,16
    ee.vst.128.ip q1,a4,16
    ee.vunzip.16 q0,q1
    ee.vzip.16 q1,q0
    ee.fft.vst.r32.decp q1,a5,0
    ee.fft.vst.r32.decp q0,a6,0
.endm
.global rrc4_unpack8
.type rrc4_unpack8,@function
rrc4_unpack8:
    entry a1,32
    beqz a7,.Lunpack_return
    rsr a8,cpenable
    movi a9,8
    or a9,a9,a8
    wsr a9,cpenable
    rsync
    ee.zero.q q4
    ee.zero.q q1
    extui a10,a2,0,4
    beqz a10,.Lunpack_aligned
    wur.sar_byte a10
    movi a9,-16
    and a2,a2,a9
    loopnez a7,.Lunpack_unaligned_end
    ee.vld.128.ip q0,a2,16
    ee.vld.128.ip q5,a2,0
    ee.src.q q0,q0,q5
    planes
.Lunpack_unaligned_end:
    j .Lunpack_restore
.Lunpack_aligned:
    loopnez a7,.Lunpack_aligned_end
    ee.vld.128.ip q0,a2,16
    planes
.Lunpack_aligned_end:
.Lunpack_restore:
    wsr a8,cpenable
    rsync
.Lunpack_return:
    retw.n
.size rrc4_unpack8,.-rrc4_unpack8

/* Independently authored bounded batch of the qualified symmetric32 dot.
 * Same VLD/SRC/VADDS/ACCX order; coeffs and CPE stay resident across outputs.
 * Center MULL fits signed32, +256 then arithmetic SRAI9 is exact floor Q6.
 * Caller owns q/SAR_BYTE/ACCX. CPE restored; scalarSAR untouched. */
.section .iram1,"ax",@progbits
.balign 4
.macro dot coeff
    wur.sar_byte a14
    ee.vld.128.ip q0,a12,16
    ee.vld.128.ip q2,a12,0
    ee.src.q q0,q0,q2
    wur.sar_byte a15
    ee.vld.128.ip q1,a13,16
    ee.vld.128.ip q3,a13,0
    ee.src.q q1,q1,q3
    ee.vadds.s16 q0,q0,q1
    ee.vmulas.s16.accx q0,\coeff
.endm
.global rrc4_batch32
.type rrc4_batch32,@function
rrc4_batch32:
    entry a1,32
    beqz a7,.Lbatch_return
    rsr a8,cpenable
    movi a9,8
    or a9,a9,a8
    wsr a9,cpenable
    rsync
    movi a15,rrc4_symbol_coeff
    ee.vld.128.ip q4,a15,16
    ee.vld.128.ip q5,a15,16
    ee.vld.128.ip q6,a15,16
    ee.vld.128.ip q7,a15,0
    movi a9,17951
    movi a11,-16
.Lbatch_batch:
    extui a14,a2,0,4
    extui a15,a3,0,4
    and a12,a2,a11
    and a13,a3,a11
    ee.zero.accx
    dot q4
    dot q5
    dot q6
    dot q7
    rur.accx_0 a10
    l16si a12,a2,64
    mull a12,a12,a9
    add a10,a10,a12
    addmi a10,a10,256
    srai a10,a10,9
    s16i a10,a6,0
    /* I/Qplanes share address phases. Preserve a14/a15 for the Q dot. */
    and a12,a4,a11
    and a13,a5,a11
    ee.zero.accx
    dot q4
    dot q5
    dot q6
    dot q7
    rur.accx_0 a10
    l16si a12,a4,64
    mull a12,a12,a9
    add a10,a10,a12
    addmi a10,a10,256
    srai a10,a10,9
    s16i a10,a6,2
    addi a2,a2,8
    addi a3,a3,-8
    addi a4,a4,8
    addi a5,a5,-8
    addi a6,a6,4
    addi a7,a7,-1
    bnez a7,.Lbatch_batch
.Lbatch_batch_end:
    wsr a8,cpenable
    rsync
.Lbatch_return:
    retw.n
.size rrc4_batch32,.-rrc4_batch32
