// Pipeline the next phase-byte load across this symbol's vector sum. Eight
// instructions/symbol plus one pointer increment per 24 symbols; no dependent
// scalar or vector load use in the steady loop. TRM v1.8 Table 1.7-2 pp66-73.
    .section .iram1.shaper_short,"ax"
    .global shaper_short4_pie
    .type shaper_short4_pie,@function
    .align 4
shaper_short4_pie:
    entry a1,32
    beqz a4,.Ldone
    movi a6,0
    movi a7,8176
    addi a9,a3,-5
    // Three old row pointers, each pointing at part one (the older triple).
    .rept 2
    l8ui a8,a9,0
    addi a9,a9,1
    addx8 a6,a6,a8
    .endr
    .macro oldrow R
    l8ui a8,a9,0
    addi a9,a9,1
    addx8 a6,a6,a8
    and a8,a6,a7
    addx2 \R,a8,a5
    addi \R,\R,16
    .endm
    oldrow a10
    oldrow a11
    oldrow a12
    movi a8,0x80200
    ee.movi.32.q q7,a8,0
    ee.movi.32.q q7,a8,1
    ee.movi.32.q q7,a8,2
    ee.movi.32.q q7,a8,3
    .macro step R,off,first=0
    addx8 a6,a6,a8
    and a9,a6,a7
    ee.vld.128.ip q1,\R,16
    addx2 \R,a9,a5
    ee.vld.128.ip q0,\R,16
    l8ui a8,a3,\off
    .if \first
    ee.vadds.s32 q0,q0,q1
    .else
    ee.vadds.s32.st.incp q4,a2,q0,q0,q1
    .endif
    ee.xorq q4,q0,q7
    .endm
    l8ui a8,a3,0
    // Emit the first symbol without a pending store, then keep q4 pending.
    // The 24-symbol loop starts with the next old row, a11.
    nop
    step a10,1,1
    addi a3,a3,1
    addi a4,a4,-1
    movi a8,24
    mov a14,a4
    quou a4,a4,a8
    addx2 a9,a4,a4
    slli a9,a9,3
    sub a14,a14,a9
    l8ui a8,a3,0
    loopnez a4,.Lend
    .set phase_offset,0
    .rept 8
    step a11,phase_offset+1
    step a12,phase_offset+2
    step a10,phase_offset+3
    .set phase_offset,phase_offset+3
    .endr
    addi a3,a3,24
.Lend:
    blti a14,3,.Ltail
.Ltriples:
    step a11,1
    step a12,2
    step a10,3
    addi a3,a3,3
    addi a14,a14,-3
    bgei a14,3,.Ltriples
.Ltail:
    beqz a14,.Lstore
    step a11,1
    addi a14,a14,-1
    beqz a14,.Lstore
    step a12,2
.Lstore:
    ee.vst.128.ip q4,a2,16
.Ldone:
    retw
    .size shaper_short4_pie,.-shaper_short4_pie
