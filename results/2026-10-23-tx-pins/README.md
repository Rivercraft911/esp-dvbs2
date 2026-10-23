# Placement pins become alignment classes

The transmitter's linker checks no longer pin absolute addresses. They pin each hot fill
kernel's position within a 32-byte fetch line, and the stream state's placement. On the board
(XIAO 44:B1:76:B0:E5:8C, radio off) every mode's longest bank fill and mean encoder time stay
within +1% of main 96c8c37, or within the run-to-run spread of 96c8c37 itself.

| Mode | Bank deadline | Max fill, 96c8c37 | Max fill, this commit | Encoder mean, 96c8c37 | Encoder mean, this commit |
|---|---:|---:|---:|---:|---:|
| `dvbpace 11` | 85,608 | 42,716 | 42,887 | 734,458 | 734,415 |
| `dvbpace 11 5m` | 85,608 | 38,979 | 38,817 | 181,622 | 181,636 |
| `dvbpace 11 10m` | 85,608 | 44,147 | 44,119 | 181,369 | 181,376 |
| `dvbpace 11 20m` | 47,220 | 37,120 | 37,086 | 147,927 | 147,964 |
| `dvbpace 11 26m667` | 47,220 | 39,431 | 39,341 | 148,156 | 148,210 |
| `dvbpace 11 33m333` | 47,220 | 41,222 | 41,229 | 154,261 | 154,440 |
| `dvbpace 4 33m333` | 47,220 | 41,731 | 41,518 | 169,286 | 169,250 |
| `dvbpace 17 5m` | 85,608 | 38,666 | 38,598 | 294,079 | 294,033 |
| `dvbpace 17 10m` | 85,608 | 43,767 to 43,953 | 43,828 to 43,910 | 254,422 to 277,082 | 254,370 to 274,878 |
| `dvbpace 15 20m` | 47,220 | 40,923 to 40,946 | 41,124 to 41,159 | 179,833 to 184,431 | 179,701 to 184,327 |
| `dvbpace 13 20m` | 47,220 | 40,913 to 40,991 | 41,187 to 41,207 | 156,761 to 157,034 | 156,840 to 156,843 |
| `dvbpace 13 26m667` | 47,220 | 40,966 to 41,561 | 40,804 to 41,346 | 170,885 to 172,551 | 170,930 to 171,491 |
| `dvbpace 15 26m667` | 47,220 | 41,030 to 41,371 | 41,244 to 41,400 | 176,011 to 178,210 | 175,675 to 178,164 |
| `dvbpace 17 26m667` | 47,220 | 40,488 to 41,648 | 40,716 to 42,106 | 153,673 to 154,343 | 154,027 to 156,097 |
| `dvbpace 7 --short` | 85,608 | 43,313 to 43,802 | 43,204 to 43,486 | 146,632 to 146,711 | 146,623 to 146,651 |
| `dvbpace 10 5m --short` | 85,608 | 38,082 to 38,871 | 38,082 to 38,887 | 54,376 to 54,382 | 54,284 to 54,288 |
| `dvbpace 16 5m --short` | 85,608 | 38,602 | 38,081 | 69,522 | 69,472 |
| `vcm 1 4 7 11`, `dvbpace 4` | 85,608 | 43,542 | 43,728 | 971,811 | 979,122 |
| `vcm mixed`, `dvbpace 4 5m` | 85,608 | 68,940 | 68,264 | 399,620 | 391,257 |

All cycles at 240 MHz; every pace ran 10 s of payload with peak 128, no late fills. Ranges are
three or four separate runs of the same build. The only systematic shift is the 8PSK 20 Msym/s
fill, +0.6% (41,2xx against 40,9xx cycles, 12.8% instead of 13.3% spare).

Run-to-run spread on 96c8c37 itself: 8PSK 9/10 at 10m alternates between encoder means near
254k, 263k and 277k depending on what ran before it (heap layout), and 8PSK 26m667 fills vary
by about 2.5%. 8PSK 2/3 and 5/6 at 26m667 already insert steady dummy frames in these 10 s
paces with Wi-Fi up (`dummy_steady` 216 to 1,174 of about 12,000), on both builds.

## What changed

- `lofty_render_pages_pie == 0x4037a324` became `& 31 == 4`, set by `.balign 32` plus 4 bytes
  instead of `.space 16` and two dummy IRAM literals in `dvbs2_fast_pl`.
- Every shaper, QPSK, short-pulse, axis and copy kernel now starts at `.balign 32` plus its
  96c8c37 offset, so later IRAM changes cannot move their loops. Without this, removing the
  padding moved them by 12 bytes and the 8PSK 20m fill rose 1.0%.
- `sizeof(dvb_t) == 0x3d0 && sizeof(encoder_t) == 0x250` became a placement check: both live in
  `.stream_bss`, 16-byte aligned internal DRAM below the DAC banks.
- The 1,632-byte lock footprint pin is gone; the 0x720 cache index of the lock stays.
- IRAM grows by 256 bytes of alignment padding (`_iram_text_end` 0x4038c800 to 0x4038c900).

## Method

`notes/matrix.py` on the host ran `dvbpace` for each mode, in the table's order, with the
callsign stored (so Morse IDs were included), first on 96c8c37 and then on this commit's tree
before committing; that build reports `96c8c37-dirty`. The repeats were run afterwards on
reflashed builds of both. `runs.jsonl` holds every record with `tag` base, base-repeat, pins or
pins-repeat.
