// Copy complete 40-word tone periods. Aligned 128-bit DAC stores, as in
// firmware/components/shaper/shaper_pie.S; alignment: ESP32-S3 TRM v1.8 1.5.3 pp. 48-49.
// Both pointers must be 16-byte aligned; periods must be positive.
    .section .iram1.station_copy, "ax"
    .global station_repeat40
    .type station_repeat40, @function
    .align 4
station_repeat40:
    entry a1, 32
    loop a4, 2f
1:
    mov a8, a3
    .rept 5
    ee.vld.128.ip q0, a8, 16
    ee.vld.128.ip q1, a8, 16
    ee.vst.128.ip q0, a2, 16
    ee.vst.128.ip q1, a2, 16
    .endr
2:
    retw.n
    .size station_repeat40, .-station_repeat40
