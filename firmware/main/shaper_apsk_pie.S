// APSK table kernels. No radio calls. TRM v1.8 table 1.7-2, pp66-73.
// A register follows one symbol/window through the rows of its table block.
// Pointer rotation avoids re-indexing nine histories at every output symbol.
// Qfrac=3 fields retain three fractional bits at the same four bytes/sample.
// A fused final add stores the previous result; the final store drains it.

    .macro constants F
    movi a15, 0x80200
    ee.movi.32.q q7, a15, 0
    ee.movi.32.q q7, a15, 1
    ee.movi.32.q q7, a15, 2
    ee.movi.32.q q7, a15, 3
    .if \F
    movi a15, 0x3ff
    ee.movi.32.q q5, a15, 0
    ee.movi.32.q q5, a15, 1
    ee.movi.32.q q5, a15, 2
    ee.movi.32.q q5, a15, 3
    movi a15, 0xffc00
    ee.movi.32.q q6, a15, 0
    ee.movi.32.q q6, a15, 1
    ee.movi.32.q q6, a15, 2
    ee.movi.32.q q6, a15, 3
    .endif
    .endm

    .macro pack F
    .if \F
    ssai 3
    ee.vsr.32 q1, q0
    ssai 9
    ee.andq q1, q1, q5
    ee.vsr.32 q2, q0
    ee.andq q2, q2, q6
    ee.orq q4, q1, q2
    ee.xorq q4, q4, q7
    .else
    ee.xorq q4, q0, q7
    .endif
    .endm

    .macro init R, OFF, AGE, N, PAIR=0, B=0
    l8ui a15, a3, \OFF
    .if \PAIR
    l8ui a14, a3, (\OFF-1)
    slli a14, a14, \B
    or a15, a15, a14
    slli \R, a15, (6+(\N/8))
    .else
    slli \R, a15, (8+(\N/8))
    .endif
    add \R, \R, a5
    movi a15, (\AGE*\N*4)
    add \R, \R, a15
    .endm

    // Pack the preceding sum while serial row loads feed the current sum.
    // Each load-use gap contains useful packing or symbol-address work.
    .macro pipeline_vec N,OFF,R0,R1,R2,R3,R4,R5,R6,R7,R8,INDEX=0
    .if \INDEX
    l8ui a15,a3,\OFF
    .endif
    ee.vld.128.ip q0,\R8,16
    .if \INDEX
    slli a15,a15,(8+(\N/8))
    .endif
    ee.vld.128.ip q1,\R7,16
    .if \INDEX
    add \R0,a15,a5
    .endif
    ee.vsr.32 q2,q3
    ee.vadds.s32.ld.incp q1,\R6,q0,q0,q1
    ee.andq q2,q2,q5
    ee.vadds.s32.ld.incp q1,\R5,q0,q0,q1
    ssai 9
    ee.vadds.s32.ld.incp q1,\R4,q0,q0,q1
    ee.vsr.32 q4,q3
    ee.vadds.s32.ld.incp q1,\R3,q0,q0,q1
    ee.andq q4,q4,q6
    ee.vadds.s32.ld.incp q1,\R2,q0,q0,q1
    ee.orq q4,q4,q2
    ee.vadds.s32.ld.incp q1,\R1,q0,q0,q1
    ee.xorq q4,q4,q7
    ee.vadds.s32.ld.incp q1,\R0,q0,q0,q1
    ssai 3
    ee.vadds.s32.st.incp q4,a2,q3,q0,q1
    .endm

    .macro single_step N, F, OFF, R0,R1,R2,R3,R4,R5,R6,R7,R8, STORE=1, PIPE=0
    .if \PIPE
    pipeline_vec \N,\OFF,\R0,\R1,\R2,\R3,\R4,\R5,\R6,\R7,\R8,1
    .if \N==8
    pipeline_vec \N,\OFF,\R0,\R1,\R2,\R3,\R4,\R5,\R6,\R7,\R8
    .endif
    .else
    l8ui a15, a3, \OFF
    slli a15, a15, (8+(\N/8))
    add \R0, a15, a5
    .set .Lvec\@, 0
    .rept (\N/4)
    ee.vld.128.ip q0, \R0, 16
    ee.vld.128.ip q1, \R1, 16
    ee.vld.128.ip q2, \R2, 16
    ee.vadds.s32.ld.incp q1, \R3, q0, q0, q1
    ee.vadds.s32.ld.incp q2, \R4, q0, q0, q2
    ee.vadds.s32.ld.incp q1, \R5, q0, q0, q1
    ee.vadds.s32.ld.incp q2, \R6, q0, q0, q2
    ee.vadds.s32.ld.incp q1, \R7, q0, q0, q1
    ee.vadds.s32.ld.incp q2, \R8, q0, q0, q2
    .if \STORE || .Lvec\@
    ee.vadds.s32.st.incp q4, a2, q0, q0, q1
    .else
    ee.vadds.s32 q0, q0, q1
    .endif
    // q2 is the last loaded row, q1 was consumed in the preceding instruction.
    // The previous line's store belongs to the preceding vector, before repacking.
    ee.vadds.s32 q0, q0, q2
    pack \F
    .set .Lvec\@, 1
    .endr
    .endif
    .endm

    .macro singles N,F, FIRST=0, PIPE=0
    .set .Lvec\@, 0
    single_step \N,\F,0,a6,a14,a13,a12,a11,a10,a9,a8,a7,(1-\FIRST),\PIPE
    single_step \N,\F,1,a7,a6,a14,a13,a12,a11,a10,a9,a8,1,\PIPE
    single_step \N,\F,2,a8,a7,a6,a14,a13,a12,a11,a10,a9,1,\PIPE
    single_step \N,\F,3,a9,a8,a7,a6,a14,a13,a12,a11,a10,1,\PIPE
    single_step \N,\F,4,a10,a9,a8,a7,a6,a14,a13,a12,a11,1,\PIPE
    single_step \N,\F,5,a11,a10,a9,a8,a7,a6,a14,a13,a12,1,\PIPE
    single_step \N,\F,6,a12,a11,a10,a9,a8,a7,a6,a14,a13,1,\PIPE
    single_step \N,\F,7,a13,a12,a11,a10,a9,a8,a7,a6,a14,1,\PIPE
    single_step \N,\F,8,a14,a13,a12,a11,a10,a9,a8,a7,a6,1,\PIPE
    addi a3,a3,9
    .endm

    .macro single N,F
    .section .iram1.apsk_single_n\N\()_f\F, "ax"
    .global apsk_single_n\N\()_f\F
    .type apsk_single_n\N\()_f\F,@function
    .align 4
apsk_single_n\N\()_f\F:
    entry a1,32
    beqz a4,9f
    constants \F
    // Address history from sym-8 because L8UI offsets are unsigned.
    addi a3,a3,-8
    init a7,0,8,\N
    init a8,1,7,\N
    init a9,2,6,\N
    init a10,3,5,\N
    init a11,4,4,\N
    init a12,5,3,\N
    init a13,6,2,\N
    init a14,7,1,\N
    addi a3,a3,8
    singles \N,\F,1
    addi a4,a4,-1
    beqz a4,2f
    .if \F
    ee.orq q3,q0,q0
    ssai 3
    .endif
1:
    singles \N,\F,0,\F
    addi a4,a4,-1
    bnez a4,1b
    .if \F
    ee.orq q0,q3,q3
    pack \F
    .endif
2:
    ee.vst.128.ip q4,a2,16
9:
    retw
    .size apsk_single_n\N\()_f\F, .-apsk_single_n\N\()_f\F
    .endm

    single 4,0
    single 8,0
    single 4,3
    single 8,3

    .macro pair_pipeline N,B,OFF,R0,R2,R4,R6
    l8ui a15,a3,(8+\OFF)
    ee.vld.128.ip q0,\R6,16
    slli a14,a14,\B
    or a14,a14,a15
    ee.vld.128.ip q1,\R4,16
    extui a15,a14,0,(2*\B)
    ee.vadds.s32.ld.incp q1,\R2,q0,q0,q1
    slli a15,a15,(6+(\N/8))
    ee.vsr.32 q2,q3
    add \R0,a15,a5
    ee.vadds.s32.ld.incp q1,\R0,q0,q0,q1
    l8ui a15,a3,\OFF
    ee.andq q2,q2,q5
    slli a15,a15,(4+(\N/8))
    ssai 9
    add a15,a15,a6
    ee.vadds.s32.ld.incp q1,a15,q0,q0,q1
    ee.vsr.32 q4,q3
    ee.andq q4,q4,q6
    ee.orq q4,q4,q2
    ee.xorq q4,q4,q7
    ssai 3
    ee.vadds.s32.st.incp q4,a2,q3,q0,q1
    .if \N==8
    ee.vld.128.ip q0,\R6,16
    ee.vld.128.ip q1,\R4,16
    ee.vsr.32 q2,q3
    ee.vadds.s32.ld.incp q1,\R2,q0,q0,q1
    ee.andq q2,q2,q5
    ee.vadds.s32.ld.incp q1,\R0,q0,q0,q1
    ssai 9
    ee.vadds.s32.ld.incp q1,a15,q0,q0,q1
    ee.vsr.32 q4,q3
    ee.andq q4,q4,q6
    ee.orq q4,q4,q2
    ee.xorq q4,q4,q7
    ssai 3
    ee.vadds.s32.st.incp q4,a2,q3,q0,q1
    .endif
    .endm

    .macro pair_step N,F,B,OFF,R0,R2,R4,R6,DEFER=0,PIPE=0
    .if \PIPE
    pair_pipeline \N,\B,\OFF,\R0,\R2,\R4,\R6
    .else
    // R0 is free until the new window enters: use it to fetch age 8 first.
    l8ui a15,a3,\OFF
    slli \R0,a15,(4+(\N/8))
    add \R0,\R0,a6
    ee.vld.128.ip q3,\R0,16
    .if \N==8
    ee.vld.128.ip q4,\R0,16
    // Preserve the previous packed vector while q4 holds the second tail vector.
    // The n8 variant uses separate stores, so q4 is free here.
    .endif
    // Symbol at a3+8+off; history is shifted once per output symbol.
    l8ui a15,a3,(8+\OFF)
    slli a14,a14,\B
    or a14,a14,a15
    extui a15,a14,0,(2*\B)
    slli a15,a15,(6+(\N/8))
    add \R0,a15,a5
    .set .Lpv\@,0
    .rept (\N/4)
    ee.vld.128.ip q0,\R0,16
    ee.vld.128.ip q1,\R2,16
    ee.vld.128.ip q2,\R4,16
    ee.vadds.s32.ld.incp q1,\R6,q0,q0,q1
    ee.vadds.s32 q0,q0,q2
    ee.vadds.s32 q0,q0,q1
    .if .Lpv\@==0
    ee.vadds.s32 q0,q0,q3
    .else
    ee.vadds.s32 q0,q0,q4
    .endif
    // Separate store: keeps q4 free for the second tail vector.
    .if \N==8
    .if .Lpv\@==0
    ee.orq q3,q4,q4
    .endif
    .endif
    pack \F
    .if !\DEFER || (\N==8 && .Lpv\@==0)
    ee.vst.128.ip q4,a2,16
    .endif
    .if \N==8
    .if .Lpv\@==0
    ee.orq q4,q3,q3
    .endif
    .endif
    .set .Lpv\@,1
    .endr
    .endif
    .endm

    .macro pairs N,F,B,DEFER=0,PIPE=0
    pair_step \N,\F,\B,0,a7,a12,a10,a8,0,\PIPE
    pair_step \N,\F,\B,1,a8,a13,a11,a9,0,\PIPE
    pair_step \N,\F,\B,2,a9,a7,a12,a10,0,\PIPE
    pair_step \N,\F,\B,3,a10,a8,a13,a11,0,\PIPE
    pair_step \N,\F,\B,4,a11,a9,a7,a12,0,\PIPE
    pair_step \N,\F,\B,5,a12,a10,a8,a13,0,\PIPE
    pair_step \N,\F,\B,6,a13,a11,a9,a7,\DEFER,\PIPE
    addi a3,a3,7
    .endm

    .macro pair M,N,F,B
    .section .iram1.apsk_pair_m\M\()_n\N\()_f\F, "ax"
    .global apsk_pair_m\M\()_n\N\()_f\F
    .type apsk_pair_m\M\()_n\N\()_f\F,@function
    .align 4
apsk_pair_m\M\()_n\N\()_f\F:
    entry a1,32
    beqz a4,9f
    constants \F
    addi a3,a3,-8
    init a8,2,3,\N,1,\B
    init a9,3,3,\N,1,\B
    init a10,4,2,\N,1,\B
    init a11,5,2,\N,1,\B
    init a12,6,1,\N,1,\B
    init a13,7,1,\N,1,\B
    l8ui a14,a3,7
    .if \F
    pairs \N,\F,\B,1
    addi a4,a4,-1
    beqz a4,2f
    ee.orq q3,q0,q0
    ssai 3
1:
    pairs \N,\F,\B,0,1
    addi a4,a4,-1
    bnez a4,1b
    ee.orq q0,q3,q3
    pack \F
2:
    ee.vst.128.ip q4,a2,16
    .else
1:
    pairs \N,\F,\B
    addi a4,a4,-1
    bnez a4,1b
    .endif
9:
    retw
    .size apsk_pair_m\M\()_n\N\()_f\F, .-apsk_pair_m\M\()_n\N\()_f\F
    .endm

    pair 16,4,0,4
    pair 16,8,0,4
    pair 16,4,3,4
    pair 16,8,3,4
    pair 32,4,0,5
    pair 32,8,0,5
    pair 32,4,3,5
    pair 32,8,3,5
