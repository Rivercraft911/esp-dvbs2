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

// Raw-phase entry points consume encoder bytes k directly. Legacy entry points still
// consume 16*k. The 4/8-sps raw path adds one scalar shift per symbol, no frame pass.
    .macro phase reg, base, off, raw
    l8ui \reg, \base, \off
    .endm

// Radix-four and radix-eight versions keep the same pipeline and row layout.
    .macro history bits
    .if \bits == 2
    addx4           a6, a6, a8
    .else
    addx8           a6, a6, a8
    .endif
    .endm

    .section .iram1.shaper4, "ax"
    .global shaper_fill4_pie
    .type   shaper_fill4_pie, @function
    .align  4

// One symbol at 4 samples/symbol. Like symbol8, six pointers rotate through the three parts.
// S already holds part 2 of this symbol. Prefetch part 2 of the next symbol into N between
// the part-0 load and its consumer, hiding the load-use stall without reading another symbol.
// That final prefetch stays within the table even on the last symbol. R is replaced by the
// current window's part-0 pointer after its part-2 row has been read. P advances through part 1.
// Nine instructions per symbol, plus one symbol-pointer advance per six symbols.
// Alignment: TRM v1.8 1.5.3, pp48-49. Fused add/load and add/store: 1.8.74-75, pp150-151.
// Load defines at M (2), add/store uses at E (1): table 1.7-2, pp68-69, so an independent
// instruction between the load and its consumer avoids the interlock (1.7.1, pp65-66).
    .macro  symbol4 R, P, next, S, N, off, raw, bits=3
    phase           a8, a3, \off, \raw
    ee.vld.128.ip   q1, \P, 16
    .if \raw
    .if \bits == 2
    srli            a8, a8, 1
    .endif
    slli            a8, a8, 4
    .endif
    history         \bits
    and             a9, a6, a7
    addx4           \R, a9, a5                  // 64 bytes/window, a9 = 16 * window
    ee.vadds.s32.ld.incp q2, \R, \S, \S, q1
    ee.vld.128.ip   \N, \next, 16
    ee.vadds.s32.st.incp q4, a2, \S, \S, q2
    ee.xorq         q4, \S, q7
    .endm

    .macro  window4 reg, off, part, raw, bits=3
    phase           a8, a9, \off, \raw
    .if \raw
    .if \bits == 2
    srli            a8, a8, 1
    .endif
    slli            a8, a8, 4
    .endif
    history         \bits
    and             a8, a6, a7
    addx4           \reg, a8, a5
    addi            \reg, \reg, \part
    .endm

// void shaper_fill4_pie(uint32_t *dst, const uint8_t *sym, int groups, const uint32_t *blocks)
// a2 dst, a3 sym, a4 groups >= 0, a5 blocks. Makes 6 * groups + 1 symbols.
    .macro kernel4 name, raw, bits=3
    .global \name
    .type \name, @function
    .align 4
\name:
    entry           a1, 32
    movi            a8, 0x80200
    ee.movi.32.q    q7, a8, 0
    ee.movi.32.q    q7, a8, 1
    ee.movi.32.q    q7, a8, 2
    ee.movi.32.q    q7, a8, 3
    movi            a7, ((1 << (3 * \bits)) - 1) * 16
    movi            a6, 0
    addi            a9, a3, -8
    phase           a8, a9, 0, \raw
    .if \raw
    .if \bits == 2
    srli            a8, a8, 1
    .endif
    slli            a8, a8, 4
    .endif
    history         \bits
    phase           a8, a9, 1, \raw
    .if \raw
    .if \bits == 2
    srli            a8, a8, 1
    .endif
    slli            a8, a8, 4
    .endif
    history         \bits
    window4         a15, 2, 32, \raw, \bits
    window4         a10, 3, 32, \raw, \bits
    window4         a11, 4, 32, \raw, \bits
    window4         a12, 5, 16, \raw, \bits
    window4         a13, 6, 16, \raw, \bits
    window4         a14, 7, 16, \raw, \bits
    // Symbol 0, without a store. Preload symbol 1's part 2 (a table row, no symbol read).
    ee.vld.128.ip   q0, a15, 16
    phase           a8, a3, 0, \raw
    ee.vld.128.ip   q1, a12, 16
    .if \raw
    .if \bits == 2
    srli            a8, a8, 1
    .endif
    slli            a8, a8, 4
    .endif
    history         \bits
    and             a9, a6, a7
    addx4           a15, a9, a5
    ee.vadds.s32.ld.incp q2, a15, q0, q0, q1
    ee.vld.128.ip   q3, a10, 16
    ee.vadds.s32    q0, q0, q2
    ee.xorq         q4, q0, q7
    addi            a3, a3, 1
    loopnez         a4, .Lend4\@
    symbol4         a10, a13, a11, q3, q0, 0, \raw, \bits
    symbol4         a11, a14, a12, q0, q3, 1, \raw, \bits
    symbol4         a12, a15, a13, q3, q0, 2, \raw, \bits
    symbol4         a13, a10, a14, q0, q3, 3, \raw, \bits
    symbol4         a14, a11, a15, q3, q0, 4, \raw, \bits
    symbol4         a15, a12, a10, q0, q3, 5, \raw, \bits
    addi            a3, a3, 6
.Lend4\@:
    ee.vst.128.ip   q4, a2, 16
    retw
    .size \name, . - \name
    .endm
    kernel4 shaper_fill4_pie, 0
    kernel4 shaper_fill4_raw_pie, 1
    kernel4 shaper_fill4_qpsk_pie, 0, 2
    kernel4 shaper_fill4_qpsk_raw_pie, 1, 2


    .section .iram1.shaper8, "ax"
    .global shaper_fill8_pie
    .type   shaper_fill8_pie, @function
    .align  4

// One symbol at 8 samples/symbol. R holds window n-6 at its part 2 row and is then reloaded
// with window n; P holds window n-3 at its part 1 row. Each register walks 32 bytes per use,
// so a window register serves part 0, then part 1 three symbols later, then part 2.
// Register use: a2 dst, a3 symbols, a5 table, a6 history (16 k per symbol, three bits apart),
// a7 window mask, a8 a9 scratch; q0 q1 sums, q2 q3 rows, q4 q5 finished words, q7 XOR mask.
    .macro  symbol8 R, P, off, raw, bits=3
    ee.vld.128.ip   q0, \R, 16                  // part 2 of window n-6, words 0-3
    phase           a8, a3, \off, \raw                // symbol n
    ee.vld.128.ip   q2, \P, 16                  // part 1 of window n-3, words 0-3
    ee.vld.128.ip   q1, \R, 16                  // part 2, words 4-7
    ee.vadds.s32.ld.incp q3, \P, q0, q0, q2     // part 1 words 4-7; q0 = part 2 + part 1
    .if \raw
    .if \bits == 2
    srli            a8, a8, 1
    .endif
    slli            a8, a8, 4
    .endif
    history         \bits                  // history = 8 history + 16 k
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
    .macro  window8 reg, off, part, raw, bits=3
    phase           a8, a9, \off, \raw
    .if \raw
    .if \bits == 2
    srli            a8, a8, 1
    .endif
    slli            a8, a8, 4
    .endif
    history         \bits
    and             a8, a6, a7
    addx8           \reg, a8, a5
    addi            \reg, \reg, \part
    .endm

// void shaper_fill8_pie(uint32_t *dst, const uint8_t *sym, int groups, const uint32_t *blocks)
// a2 dst, a3 sym, a4 groups, a5 blocks. Makes 6 * groups + 1 symbols.
    .macro kernel8 name, raw, bits=3
    .global \name
    .type \name, @function
    .align 4
\name:
    entry           a1, 32
    movi            a8, 0x80200
    ee.movi.32.q    q7, a8, 0
    ee.movi.32.q    q7, a8, 1
    ee.movi.32.q    q7, a8, 2
    ee.movi.32.q    q7, a8, 3
    movi            a7, ((1 << (3 * \bits)) - 1) * 16
    movi            a6, 0
    addi            a9, a3, -8
    phase           a8, a9, 0, \raw
    .if \raw
    .if \bits == 2
    srli            a8, a8, 1
    .endif
    slli            a8, a8, 4
    .endif
    history         \bits
    phase           a8, a9, 1, \raw
    .if \raw
    .if \bits == 2
    srli            a8, a8, 1
    .endif
    slli            a8, a8, 4
    .endif
    history         \bits
    // Windows of symbols -6..-1: three at part 2 (symbols 0..2 read them), three at part 1.
    window8         a15, 2, 64, \raw, \bits
    window8         a10, 3, 64, \raw, \bits
    window8         a11, 4, 64, \raw, \bits
    window8         a12, 5, 32, \raw, \bits
    window8         a13, 6, 32, \raw, \bits
    window8         a14, 7, 32, \raw, \bits
    // Symbol 0 (registers of rotation step 5) without a store.
    ee.vld.128.ip   q0, a15, 16
    phase           a8, a3, 0, \raw
    ee.vld.128.ip   q2, a12, 16
    ee.vld.128.ip   q1, a15, 16
    ee.vadds.s32.ld.incp q3, a12, q0, q0, q2
    .if \raw
    .if \bits == 2
    srli            a8, a8, 1
    .endif
    slli            a8, a8, 4
    .endif
    history         \bits
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
    loopnez         a4, .Lend8\@
    symbol8         a10, a13, 0, \raw, \bits
    symbol8         a11, a14, 1, \raw, \bits
    symbol8         a12, a15, 2, \raw, \bits
    symbol8         a13, a10, 3, \raw, \bits
    symbol8         a14, a11, 4, \raw, \bits
    symbol8         a15, a12, 5, \raw, \bits
    addi            a3, a3, 6
.Lend8\@:
    ee.vst.128.ip   q4, a2, 16
    ee.vst.128.ip   q5, a2, 16
    retw
    .size \name, . - \name
    .endm
    kernel8 shaper_fill8_pie, 0
    kernel8 shaper_fill8_raw_pie, 1
    kernel8 shaper_fill8_qpsk_pie, 0, 2
    kernel8 shaper_fill8_qpsk_raw_pie, 1, 2


    .section .iram1.shaper40, "ax"
    .global shaper_fill40_pie
    .type   shaper_fill40_pie, @function
    .align  4

// Next symbol's three row pointers at 40 samples/symbol, 2-bit codes, 512-byte blocks:
// a9 part 0 of window n, a10 part 1 of window n-3 (a13 = table + 160), a11 part 2 of window
// n-6 (a14 = table + 320). a6 history (2 bits per symbol), a7 a8 scratch.
    .macro  rows40 raw
    l8ui            a7, a3, 0
    addi            a3, a3, 1
    srli            a7, a7, (5 - 4 * \raw)                   // code = k >> 1
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

    .macro  code40 off, raw
    l8ui            a7, a9, \off
    srli            a7, a7, (5 - 4 * \raw)
    addx4           a6, a6, a7
    .endm

// void shaper_fill40_pie(uint32_t *dst, const uint8_t *sym, int nsym, const uint32_t *blocks)
// a2 dst, a3 sym, a4 nsym >= 1, a5 blocks.
    .macro kernel40 name, raw
    .global \name
    .type \name, @function
    .align 4
\name:
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
    code40          0, \raw
    code40          1, \raw
    code40          2, \raw
    code40          3, \raw
    code40          4, \raw
    code40          5, \raw
    code40          6, \raw
    code40          7, \raw
    rows40 \raw
    pair40          "ee.vadds.s32 q0, q0, q1", "ee.vadds.s32 q2, q2, q6"
    movi            a12, 4
    j               .Lrow40\@
.Lsym40\@:
    rows40 \raw
    movi            a12, 5
.Lrow40\@:
    loop            a12, .Lrow40_end\@
    pair40          "ee.vadds.s32.st.incp q4, a2, q0, q0, q1", "ee.vadds.s32.st.incp q5, a2, q2, q2, q6"
.Lrow40_end\@:
    addi            a4, a4, -1
    bnez            a4, .Lsym40\@
    ee.vst.128.ip   q4, a2, 16
    ee.vst.128.ip   q5, a2, 16
    retw
    .size \name, . - \name
    .endm
    kernel40 shaper_fill40_pie, 0
    kernel40 shaper_fill40_raw_pie, 1

    .section .text.shaper4rows, "ax"
    .global shaper_fill4_rows_pie
    .type   shaper_fill4_rows_pie, @function
    .align  4

// Staged internal row pointers replace symbol load, history update, mask and address.
// a3 points to rows[0], rows[-6..-1] supply history, rows[nsym] is a valid guard pointer.
// Next part 1 and row address are prefetched to hide both load-use stalls. Six instructions/symbol
// plus one pointer advance per six symbols. Same prefetch schedule and table as n4.
    .macro symbol4rows R, next, nextP, S, N, off
    ee.vadds.s32.ld.incp q2, \R, \S, \S, q1
    ee.vld.128.ip   \N, \next, 16
    l32i            \next, a3, \off
    ee.vadds.s32.st.incp q4, a2, \S, \S, q2
    ee.vld.128.ip   q1, \nextP, 16
    ee.xorq         q4, \S, q7
    .endm

    .macro window4rows reg, off, part
    l32i            \reg, a3, \off
    addi            \reg, \reg, \part
    .endm

// dst a2, rows a3, groups a4. No table argument.
shaper_fill4_rows_pie:
    entry           a1, 32
    movi            a8, 0x80200
    ee.movi.32.q    q7, a8, 0
    ee.movi.32.q    q7, a8, 1
    ee.movi.32.q    q7, a8, 2
    ee.movi.32.q    q7, a8, 3
    addi            a3, a3, -24
    window4rows     a15, 0, 32
    window4rows     a10, 4, 32
    window4rows     a11, 8, 32
    window4rows     a12, 12, 16
    window4rows     a13, 16, 16
    window4rows     a14, 20, 16
    addi            a3, a3, 24
    // Symbol 0, without a store. Preload symbol 1's part 2 (a table row, no symbol read).
    ee.vld.128.ip   q0, a15, 16
    l32i            a15, a3, 0
    ee.vld.128.ip   q1, a12, 16
    ee.vadds.s32.ld.incp q2, a15, q0, q0, q1
    ee.vld.128.ip   q3, a10, 16
    ee.vadds.s32    q0, q0, q2
    ee.xorq         q4, q0, q7
    ee.vld.128.ip   q1, a13, 16
    l32i            a10, a3, 4
    addi            a3, a3, 4
    loopnez         a4, .Lend4rows
    symbol4rows a10, a11, a14, q3, q0, 4
    symbol4rows a11, a12, a15, q0, q3, 8
    symbol4rows a12, a13, a10, q3, q0, 12
    symbol4rows a13, a14, a11, q0, q3, 16
    symbol4rows a14, a15, a12, q3, q0, 20
    symbol4rows a15, a10, a13, q0, q3, 24
    addi            a3, a3, 24
.Lend4rows:
    ee.vst.128.ip   q4, a2, 16
    retw
    .size   shaper_fill4_rows_pie, . - shaper_fill4_rows_pie



// Five samples/symbol prototype. Each part row is padded to eight words. Compute two
// vectors, retain five lanes, and pack four symbols into five aligned stores. SAR_BYTE
// selects bytes from the concatenation (TRM v1.8 1.8.49 p125, 1.8.56 p132). No unaligned memory access.
    .section .text.shaper5, "ax"
    .global shaper_fill5_pie
    .type shaper_fill5_pie, @function
    .align 4
    .macro calc5 R, P, off
    ee.vld.128.ip q0, \R, 16
    l8ui a8, a3, \off
    ee.vld.128.ip q2, \P, 16
    ee.vld.128.ip q1, \R, 16
    ee.vadds.s32.ld.incp q3, \P, q0, q0, q2
    addx8 a6, a6, a8
    and a9, a6, a7
    addx8 \R, a9, a5
    ee.vadds.s32.ld.incp q2, \R, q1, q1, q3
    ee.vld.128.ip q3, \R, 16
    ee.vadds.s32 q0, q0, q2
    ee.vadds.s32 q1, q1, q3
    ee.xorq q4, q0, q7
    ee.xorq q5, q1, q7
    .endm

    .macro pack5 shift, remain
    movi a8, \shift
    wur.sar_byte a8
    ee.src.q q6, q6, q6
    movi a8, \remain
    wur.sar_byte a8
    ee.srcq.128.st.incp q6, q4, a2
    ee.src.q q6, q4, q5
    .endm

// Six window registers rotate; twelve symbols restore both the pointer and packing phases.
    .macro group5 R0,P0,R1,P1,R2,P2,R3,P3,off
    calc5 \R0, \P0, \off
    ee.vst.128.ip q4, a2, 16
    ee.orq q6, q5, q5
    calc5 \R1, \P1, (\off+1)
    pack5 4, 12
    calc5 \R2, \P2, (\off+2)
    pack5 8, 8
    calc5 \R3, \P3, (\off+3)
    pack5 12, 4
    ee.srcq.128.st.incp q4, q5, a2
    .endm

shaper_fill5_pie:
    entry a1, 32
    beqz a4, .Ldone5
    movi a8, 0x80200
    ee.movi.32.q q7, a8, 0
    ee.movi.32.q q7, a8, 1
    ee.movi.32.q q7, a8, 2
    ee.movi.32.q q7, a8, 3
    movi a7, 0x1ff0
    movi a6, 0
    addi a9, a3, -8
    l8ui a8, a9, 0
    addx8 a6, a6, a8
    l8ui a8, a9, 1
    addx8 a6, a6, a8
    window8 a15, 2, 64, 0
    window8 a10, 3, 64, 0
    window8 a11, 4, 64, 0
    window8 a12, 5, 32, 0
    window8 a13, 6, 32, 0
    window8 a14, 7, 32, 0
.Lgroup5a:
    group5 a15,a12,a10,a13,a11,a14,a12,a15,0
    addi a3, a3, 4
    addi a4, a4, -1
    beqz a4, .Ldone5
    group5 a13,a10,a14,a11,a15,a12,a10,a13,0
    addi a3, a3, 4
    addi a4, a4, -1
    beqz a4, .Ldone5
    group5 a11,a14,a12,a15,a13,a10,a14,a11,0
    addi a3, a3, 4
    addi a4, a4, -1
    bnez a4, .Lgroup5a
.Ldone5:
    retw
    .size shaper_fill5_pie, . - shaper_fill5_pie


    .section .iram1.unpack, "ax"
    .global shaper_unpack_pie
    .type   shaper_unpack_pie, @function
    .align  4

// Packed ring frames (dvbs2_pack: two phases per byte, low nibble first) back to one phase per
// byte. Each pass reads 16 bytes at any alignment, as dvbs2_pie_index8 does: EE.LD.128.USAR.IP
// and the next aligned vector, joined by EE.SRC.Q (TRM v1.8 1.8.17, 1.8.49), so up to 16 bytes
// after the last block are read. The low nibbles are the even phases, the high nibbles (shifted
// down, then masked) the odd ones, and EE.VZIP.8 (1.8.212) interleaves them into 32 phases.
// void shaper_unpack_pie(uint8_t *dst, const uint8_t *src, unsigned blocks), dst 16-byte aligned.
    .macro unpack name, codes
    .global \name
    .type \name, @function
    .align 4
\name:
    entry           a1, 32
    .if \codes == 2
    movi            a8, 0x30303030
    .elseif \codes
    movi            a8, 0xf0f0f0f0
    .else
    movi            a8, 0x0f0f0f0f
    .endif
    ee.movi.32.q    q7, a8, 0
    ee.movi.32.q    q7, a8, 1
    ee.movi.32.q    q7, a8, 2
    ee.movi.32.q    q7, a8, 3
    ssai            4
    loopnez         a4, .Lunpack_end\@
    ee.ld.128.usar.ip q0, a3, 16
    ee.vld.128.ip   q1, a3, 0
    ee.src.q        q0, q0, q1                  // 16 packed bytes
    .if \codes == 2
    ssai            3
    ee.vsl.32       q1, q0
    ssai            1
    ee.vsr.32       q0, q0
    .elseif \codes
    ee.vsl.32       q1, q0                      // even phases up; odd phases already in place
    .else
    ee.vsr.32       q1, q0                      // high nibbles down; the mask drops the rest
    .endif
    ee.andq         q0, q0, q7                  // phases 0, 2, ..., 30
    ee.andq         q1, q1, q7                  // phases 1, 3, ..., 31
    .if \codes
    ee.vzip.8       q1, q0
    ee.vst.128.ip   q1, a2, 16
    ee.vst.128.ip   q0, a2, 16
    .else
    ee.vzip.8       q0, q1                      // phases 0..15 and 16..31
    ee.vst.128.ip   q0, a2, 16
    ee.vst.128.ip   q1, a2, 16
    .endif
.Lunpack_end\@:
    retw
    .size \name, . - \name
    .endm
    unpack shaper_unpack_pie, 0
    unpack shaper_unpack_codes_pie, 1
    unpack shaper_unpack_qpsk_pie, 2

// Disjoint aligned word buffers, eight words per pair. Independent loads separate each
// load from its consumer (TRM v1.8 1.7.1, pp65-66; loads 1.8.88 p164, stores 1.8.192 p275).
    .section .iram1.shaper_copy, "ax"
    .global shaper_copy_pie
    .type shaper_copy_pie, @function
    .align 4
shaper_copy_pie:
    entry a1, 32
    loopnez a4, .Lcopy_end
    ee.vld.128.ip q0, a3, 16
    ee.vld.128.ip q1, a3, 16
    ee.vst.128.ip q0, a2, 16
    ee.vst.128.ip q1, a2, 16
.Lcopy_end:
    retw
    .size shaper_copy_pie, . - shaper_copy_pie
