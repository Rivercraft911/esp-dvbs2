// TRM v1.8 1.8.88 p164: VLD forces address low four bits to zero.
// Keep four disposable history bits below each eight-bit index. EXTUI then
// produces a byte offset directly. Final signed DAC fields are in the tables.
    .section .iram1.machine,"ax"
    .global machine_render_pie
    .type machine_render_pie,@function
    .align 4
machine_render_pie:
    entry a1,32
    beqz a4,.Ldone
    l32i a11,a3,0
    movi a6,4096
    add a6,a6,a5
    movi a7,8192
    add a7,a7,a5
    movi a8,12288
    add a8,a8,a5
    movi a9,16384
    add a9,a9,a5
    movi a10,20480
    add a10,a10,a5
    loopnez a4,.Lend
    extui a12,a11,3,12
    extui a13,a11,19,12
    add a12,a12,a5
    add a13,a13,a8
    ee.vld.128.ip q0,a12,0
    extui a14,a11,2,12
    ee.vld.128.ip q1,a13,0
    extui a15,a11,18,12
    add a14,a14,a6
    ee.orq q0,q0,q1
    add a15,a15,a9
    ee.vld.128.ip q2,a14,0
    ee.vst.128.ip q0,a2,16
    ee.vld.128.ip q3,a15,0
    extui a12,a11,0,12
    extui a13,a11,16,12
    add a12,a12,a7
    add a13,a13,a10
    ee.orq q2,q2,q3
    ee.vld.128.ip q0,a12,0
    ee.vst.128.ip q2,a2,16
    ee.vld.128.ip q1,a13,0
    addi a3,a3,4
    l32i a11,a3,0 // one readable source-word guard; hides load-use latency
    ee.orq q0,q0,q1
    ee.vst.128.ip q0,a2,16
.Lend:
.Ldone:
    retw
    .size machine_render_pie,.-machine_render_pie


// Packed bytes, four five-symbol blocks per five bytes. A 4 KiB LUT separates
// the newest five I/Q signs; the previous six signs stay in scalar registers.
// Unlike the wild-a probe, timing includes extracting groups from quad bytes.
    .macro byte off
    l8ui a9,a3,\off
    slli a8,a8,8
    or a8,a8,a9
    .endm
    .macro signs shift,off
    extui a10,a8,\shift,10
    slli a11,a11,5
    addx4 a10,a10,a5
    and a11,a11,a14
    l32i a10,a10,0
    or a11,a11,a10
    s32i a11,a2,\off
    .endm
    .global machine_prepare_pie
    .type machine_prepare_pie,@function
    .align 4
machine_prepare_pie:
    entry a1,32
    beqz a4,.Lprepare_done
    l8ui a8,a3,0
    l8ui a9,a3,1
    slli a8,a8,8
    or a8,a8,a9
    // First six chronological symbols initialize the rolling history.
    extui a10,a8,6,10
    extui a12,a8,4,2
    addx4 a10,a10,a5
    addx4 a12,a12,a5
    l32i a11,a10,0
    l32i a12,a12,0
    slli a11,a11,1
    or a11,a11,a12
    movi a14,0x7ff07ff0
    loopnez a4,.Lprepare_end
    byte 2
    signs 2,0
    byte 3
    signs 0,4
    byte 4
    byte 5
    signs 6,8
    byte 6
    signs 4,12
    addi a3,a3,5
    addi a2,a2,16
.Lprepare_end:
.Lprepare_done:
    retw
    .size machine_prepare_pie,.-machine_prepare_pie
