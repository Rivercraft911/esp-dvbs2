// Pulse-shaping kernels on the ESP32-S3 PIE vector unit (ESP32-S3 TRM v1.8, chapter 1).
// Table layout and the bit-exact model: shaper.h and host/shaper.py.
//
// Per output vector (four DAC words) the kernels load three part rows, add them with the
// saturating 32-bit add (the offset-binary fields never come near saturation), XOR 0x80200 and
// store. Adds ride on loads and stores (EE.VADDS.S32.LD.INCP / .ST.INCP), so a vector costs
// three loads, one store and one XOR: five instructions. Stores lag one step behind: each step
// stores the previous step's result (q4, q5), the prologue makes the first result and the
// epilogue stores the last. Instructions are ordered so that no loaded register is read by the
// next instruction (load results arrive one cycle later than ALU results, TRM table 1.7-2).

    .section .iram1.shaper8, "ax"
    .global shaper_fill8_pie
    .type   shaper_fill8_pie, @function
    .align  4

// One symbol at 8 samples/symbol. R holds window n-6 at its part 2 row and is then reloaded
// with window n; P holds window n-3 at its part 1 row. Each register walks 32 bytes per use,
// so a window register serves part 0, then part 1 three symbols later, then part 2.
// Register use: a2 dst, a3 symbols, a5 table, a6 history (16 k per symbol, three bits apart),
// a7 window mask, a8 a9 scratch; q0 q1 sums, q2 q3 rows, q4 q5 finished words, q7 XOR mask.
    .macro  symbol8 R, P, off
    ee.vld.128.ip   q0, \R, 16                  // part 2 of window n-6, words 0-3
    l8ui            a8, a3, \off                // symbol n
    ee.vld.128.ip   q2, \P, 16                  // part 1 of window n-3, words 0-3
    ee.vld.128.ip   q1, \R, 16                  // part 2, words 4-7
    ee.vadds.s32.ld.incp q3, \P, q0, q0, q2     // part 1 words 4-7; q0 = part 2 + part 1
    addx8           a6, a6, a8                  // history = 8 history + 16 k
    and             a9, a6, a7                  // 16 * window n
    addx8           \R, a9, a5                  // table + 128 * window n
    ee.vadds.s32.ld.incp q2, \R, q1, q1, q3     // part 0 words 0-3; q1 = part 2 + part 1
    ee.vld.128.ip   q3, \R, 16                  // part 0 words 4-7
    ee.vadds.s32.st.incp q4, a2, q0, q0, q2     // store previous words 0-3; q0 += part 0
    ee.vadds.s32.st.incp q5, a2, q1, q1, q3     // store previous words 4-7; q1 += part 0
    ee.xorq         q4, q0, q7                  // to two's complement
    ee.xorq         q5, q1, q7
    .endm

// The prologue's window setup: shift symbol sym[-8 + off] into the history and point reg at
// its window block plus part bytes.
    .macro  window8 reg, off, part
    l8ui            a8, a9, \off
    addx8           a6, a6, a8
    and             a8, a6, a7
    addx8           \reg, a8, a5
    addi            \reg, \reg, \part
    .endm

// void shaper_fill8_pie(uint32_t *dst, const uint8_t *sym, int groups, const uint32_t *blocks)
// a2 dst, a3 sym, a4 groups, a5 blocks. Makes 6 * groups + 1 symbols.
shaper_fill8_pie:
    entry           a1, 32
    movi            a8, 0x80200
    ee.movi.32.q    q7, a8, 0
    ee.movi.32.q    q7, a8, 1
    ee.movi.32.q    q7, a8, 2
    ee.movi.32.q    q7, a8, 3
    movi            a7, 0x1ff0
    movi            a6, 0
    addi            a9, a3, -8
    l8ui            a8, a9, 0
    addx8           a6, a6, a8
    l8ui            a8, a9, 1
    addx8           a6, a6, a8
    // Windows of symbols -6..-1: three at part 2 (symbols 0..2 read them), three at part 1.
    window8         a15, 2, 64
    window8         a10, 3, 64
    window8         a11, 4, 64
    window8         a12, 5, 32
    window8         a13, 6, 32
    window8         a14, 7, 32
    // Symbol 0 (registers of rotation step 5) without a store.
    ee.vld.128.ip   q0, a15, 16
    l8ui            a8, a3, 0
    ee.vld.128.ip   q2, a12, 16
    ee.vld.128.ip   q1, a15, 16
    ee.vadds.s32.ld.incp q3, a12, q0, q0, q2
    addx8           a6, a6, a8
    and             a9, a6, a7
    addx8           a15, a9, a5
    ee.vadds.s32.ld.incp q2, a15, q1, q1, q3
    ee.vld.128.ip   q3, a15, 16
    ee.vadds.s32    q0, q0, q2
    ee.vadds.s32    q1, q1, q3
    ee.xorq         q4, q0, q7
    ee.xorq         q5, q1, q7
    addi            a3, a3, 1
    // Six symbols per pass: the six window registers rotate once.
    loopnez         a4, .Lend8
    symbol8         a10, a13, 0
    symbol8         a11, a14, 1
    symbol8         a12, a15, 2
    symbol8         a13, a10, 3
    symbol8         a14, a11, 4
    symbol8         a15, a12, 5
    addi            a3, a3, 6
.Lend8:
    ee.vst.128.ip   q4, a2, 16
    ee.vst.128.ip   q5, a2, 16
    retw
    .size   shaper_fill8_pie, . - shaper_fill8_pie


    .section .iram1.shaper40, "ax"
    .global shaper_fill40_pie
    .type   shaper_fill40_pie, @function
    .align  4

// Next symbol's three row pointers at 40 samples/symbol, 2-bit codes, 512-byte blocks:
// a9 part 0 of window n, a10 part 1 of window n-3 (a13 = table + 160), a11 part 2 of window
// n-6 (a14 = table + 320). a6 history (2 bits per symbol), a7 a8 scratch.
    .macro  rows40
    l8ui            a7, a3, 0
    addi            a3, a3, 1
    srli            a7, a7, 5                   // code = k >> 1
    addx4           a6, a6, a7
    extui           a8, a6, 0, 6
    slli            a8, a8, 9
    add             a9, a8, a5
    extui           a8, a6, 6, 6
    slli            a8, a8, 9
    add             a10, a8, a13
    extui           a8, a6, 12, 6
    slli            a8, a8, 9
    add             a11, a8, a14
    .endm

// Two vectors (8 words) of a 40-word row; the last two lines store the previous two vectors.
    .macro  pair40 add0, add1
    ee.vld.128.ip   q0, a9, 16
    ee.vld.128.ip   q1, a10, 16
    ee.vld.128.ip   q2, a9, 16
    ee.vadds.s32.ld.incp q3, a10, q0, q0, q1
    ee.vld.128.ip   q1, a11, 16
    ee.vadds.s32.ld.incp q6, a11, q2, q2, q3
    \add0
    \add1
    ee.xorq         q4, q0, q7
    ee.xorq         q5, q2, q7
    .endm

    .macro  code40 off
    l8ui            a7, a9, \off
    srli            a7, a7, 5
    addx4           a6, a6, a7
    .endm

// void shaper_fill40_pie(uint32_t *dst, const uint8_t *sym, int nsym, const uint32_t *blocks)
// a2 dst, a3 sym, a4 nsym >= 1, a5 blocks.
shaper_fill40_pie:
    entry           a1, 32
    movi            a8, 0x80200
    ee.movi.32.q    q7, a8, 0
    ee.movi.32.q    q7, a8, 1
    ee.movi.32.q    q7, a8, 2
    ee.movi.32.q    q7, a8, 3
    movi            a13, 160
    add             a13, a13, a5
    movi            a14, 320
    add             a14, a14, a5
    movi            a6, 0
    addi            a9, a3, -8
    code40          0
    code40          1
    code40          2
    code40          3
    code40          4
    code40          5
    code40          6
    code40          7
    rows40
    pair40          "ee.vadds.s32 q0, q0, q1", "ee.vadds.s32 q2, q2, q6"
    movi            a12, 4
    j               .Lrow40
.Lsym40:
    rows40
    movi            a12, 5
.Lrow40:
    loop            a12, .Lrow40_end
    pair40          "ee.vadds.s32.st.incp q4, a2, q0, q0, q1", "ee.vadds.s32.st.incp q5, a2, q2, q2, q6"
.Lrow40_end:
    addi            a4, a4, -1
    bnez            a4, .Lsym40
    ee.vst.128.ip   q4, a2, 16
    ee.vst.128.ip   q5, a2, 16
    retw
    .size   shaper_fill40_pie, . - shaper_fill40_pie
