# Wide transmit filter

This branch applies the PHY bandwidth selector after CW bring-up. Its RF response is
**unmeasured on this board**. Default `filter auto` selects full mode 2 for total occupied
bandwidth above 20 MHz, otherwise mode 0. The threshold comes from the earlier observed
roughly +/-10 MHz response [over the air](../results/2026-10-16-radio/README.md), including
an unknown antenna response. This is a candidate default, pending the tests below.

With root-raised-cosine rolloff alpha = 0.35, nominal support is
`B = Rs * (1 + alpha) = (DAC rate / samples per symbol) * 1.35` in Hz.
Thus 10, 20 and 26.666667 Msym/s need 13.5, 27 and 36 MHz respectively.
Firmware rounds B upward to integer Hz. Finite shaping adds out-of-band sidelobes;
B is a selection input, not measured occupied bandwidth. At B = 20 MHz auto stays narrow;
at B = 20 MHz + 1 Hz it selects wide. DAC rate alone does not choose the filter.

`filter auto|narrow|wide|analog` changes RAM configuration for later keyed commands. It does
not start Wi-Fi or access PHY registers. Reset restores auto. Keep `chain cwgain` for RF.
`cw40` is retained for historical comparisons: its packet-test flag does not select CW width.
The ordinary `tx` signals report their spectral span, including space for the station ID.
Older component run APIs without a bandwidth argument conservatively use the full DAC band.
USB dumps, paced simulation and `wifimem` do not apply or read these filter registers.

## Implementation and evidence

Installed ESP-IDF v6.1 development checkout `fff9895c82d744c7237be8847347bdd1b07c6643`,
PHY libraries `5695f4f38108658bc4a33e4712c1ebcb34911434`, S3 rev0 ROM ELF:

- `esp_phy_wifi_tx_tone` reaches `wifiscwout`, then `rftest_set_chan(channel,0)`.
  `esp_phy_cbw40m_en` only sets a packet-test flag. Confirmed by the installed
  `librftest.a` relocations/instructions, not RF measurement.
- After `stop_tx_tone(1)`, `phy_set_freq` and gain pinning, wide calls the IDF linker symbol
  `rom_bb_bss_cbw40(2)` (veneer `0x40005fdc`, ROM body `0x40039074`). Narrow calls it with 0.
  Analog calls mode 0 followed by `rom_bb_bss_cbw40_ana(1)` (veneer `0x400061c8`,
  body `0x40035dc4`). Mode 2 includes digital/channel settings as well as analog width.
- The ROM instruction trace predicts these readbacks. Other bits are preserved by the ROM;
  mode 0 does not write the two fields marked untouched.

| JSON field, MMIO address | Mask | narrow | wide | analog |
|---|---|---|---|---|
| analog, `0x60006100` | `0x003f0000` | 0 | `0x00120000` | `0x00120000` |
| digital, `0x6002600c` | `0x0000000c` | 0 | 4 | 0 |
| channel, `0x60033400` | 3 | 0 | 3 | 0 |
| cbw, `0x6001d000` | `0x40` | untouched | 0 | untouched |
| mac, `0x6001cc0c` | 1 | untouched | 0 | untouched |

Each keyed `tx`/`dvb` record includes `filter.requested`, `selected`, `occupied_hz`, `applied`,
`matches`, `retained`, and three snapshots: `before` (after CW/gain, before filter selection),
`active` (before DAC playback), `end` (DAC stopped, before PHY cleanup). Raw MMIO words are
unsigned decimal. `caps` is BBTOP block `0x67`, registers **[4,5,6,7,12,13,14,15]** in that
order, raw bytes; mask each with 63 for its capacitor code. The first four concern RX; the last
four are TX pairs A and B. No capacitor code is changed by our code.
A selector mismatch aborts before DAC playback; cleanup unkeys. End selector mismatch or any
capacitor-byte change fails the run. Snapshots cannot detect a transient change mid-run.
Readback agreement establishes configuration only. The analog field has been described as
read-only in prior-art shell experiments, so disagreement is useful evidence, not something
to suppress. Source trace and hashes remain in the original ignored
`notes/wide-filter/wide-filter-sources/` in the main checkout.

[Prior-art filter register findings](prior-art.md) distinguish the TX and RX pairs and cite
[the pinned register document](https://github.com/cyber-murmel/esp-sdr-zephyr/blob/3214c16a7c9f136414cc5ae9d245ea290c98a6ce/doc/undocumented-registers-esp32s3.md#L1189-L1251).
Its reported mode-2 response through +/-18 MHz includes both S3 radios. It is a comparison
target, not our TX specification. No prior-art implementation is incorporated.

## Can the S3 measure itself?

Raw RX I/Q capture is credible. [ESPARGOS's S3 receiver](https://github.com/ESPARGOS/esp-sdr/blob/main/main/targets/esp32s3/receiver.c)
uses the dump engine, and the pinned [register notes](https://github.com/cyber-murmel/esp-sdr-zephyr/blob/3214c16a7c9f136414cc5ae9d245ea290c98a6ce/doc/undocumented-registers-esp32s3.md#L192-L246)
describe an 80 MS/s one-shot capture, about 204.75 us for 16380 complex samples.
That does not establish simultaneous capture of our DAC playback.

The documented RX and TX engines use the **same** one-hot SRAM bank selector
`0x600c101c[3:0]` ([bank notes](https://github.com/cyber-murmel/esp-sdr-zephyr/blob/3214c16a7c9f136414cc5ae9d245ea290c98a6ce/doc/undocumented-registers-esp32s3.md#L343-L383)).
Reserving another bank does not give each engine an independent bank address. Starting a
capture during playback could overwrite the samples being transmitted. No independently
addressed simultaneous path is established in the evidence examined.

The vendor ROM does have `rom_loopback_mode_en` (veneer `0x40005da8`, body `0x40038194`):
it writes BBTOP `0x67:0[6]`, then enables TX and RX clocks through PHY table offsets 0x44/0x48.
`rom_set_loopback_gain` is at `0x40005ef8`. The RAM `rfcal_txiq` uses loopback in one branch;
`txiq_get_mis_pwr` reads scalar `txtone_linear_pwr` measurements from calibration tones.
This establishes a calibration control, **not** the loopback tap's position relative to the TX
filter, mixer or PA, nor raw ADC visibility of that tap. No public schematic was found that
settles it. A baseband loop may bypass the image/carrier mechanisms we want to test.

Conclusion: no qualified self-spectrum command is feasible from the established controls
without board experiments. No speculative loopback command is shipped. This is an evidence
limit, not proof that silicon cannot do it. A later experiment can avoid the shared-DAC-bank
problem by using the hardware tone generator plus ADC dump, after proving the tone traverses
the wanted filter and mapping the loopback tap. It must measure a two-filter cascade
`P(f) = Pin(f) * |Htx(f) Hloop(f) Hrx(f)|^2`; a repeatable ratio with RX/loop unchanged can
self-test TX changes, but absolute TX width needs de-embedding or an external reference.
It must also show loopback on/off discrimination, no clipping, full sentinel replacement,
ADC done/count checks, bounded timeout and complete restoration/unkeying on every exit.
Only the coordinator should perform that keyed investigation in the box.

## Coordinator checks, radio off

No board was opened, reset, flashed or keyed during implementation. Host gates:

```sh
python3 tests/test_radio_filter.py
python3 tests/test_radio.py
python3 tests/test_tx_command.py
python3 tests/test_filter_response.py
cd firmware
. ~/.espressif/tools/activate_idf_v6.1.sh >/dev/null 2>&1
idf.py build
```

In the restricted agent sandbox only, the optional IDF component manager needs
`IDF_COMPONENT_MANAGER=0` because its process enumeration is blocked. No managed components
are used. Native compiler tests use ROM/MMIO fakes; synthetic spectra do not qualify hardware.

After the coordinator builds/flashes, retain `info`, firmware hash, binary SHA-256, full commands
and records in a new `results/<date>-widefilter/`. Do not edit past results. A slim regression:
16-frame dumps `11 26m667`, `4 20mq4`, plus `4 1m` and `13 5m` regression dumps; existing
[QPSK checks](qpsk-goal.md) supply exact CRCs and LeanDVB invocation (use legacy `dvbdump 4 40`
for the 1 Msym/s regression). Require CRC equality, zero overlap errors, >=frames-2 contiguous
bit-exact payloads, no LDPC flips/check failures or BCH corrections/failures. Samples are
unchanged by filter controls. `dvbpace 11 26m667 30`: zero underruns, late fills, fatal misses;
positive slack. `wifimem 11 26m667` and `wifimem 8 20mq4`: start/stop only, zero playback,
internal free-up/low-water >=30000 B and >=20000 B respectively. Changing `filter` before these
must not cause register access. Keep board work within the coordinator's usual regression set.

## Coordinator checks, keyed inside the box

Use licensed spectrum, dummy load or >=30 dB attenuation into E200 RX1, manual receive gain,
and a recorded fixed RF path. Never connect TX directly to RX. The coordinator owns keying.
2420 MHz +/-20 MHz fits the 2390..2450 MHz command range and the applicable 13 cm allocation;
confirm the station's licensed allocation. Record attenuation, gain, cabling and E200 identity.
This E200 contains AD9363 running in AD9364 mode. ADI specifies AD9363 for 20 MHz RF bandwidth;
[AD9364 supports 56 MHz](https://wiki.analog.com/resources/eval/user-guides/ad9361).
The software personality/readback does not qualify this particular AD9363 above its rating.
61.44 MS/s finite bursts are gap-free per the coordinator; no continuous Ethernet stream is
needed. At 4,194,304 complex samples a burst is 68.267 ms and 16,777,216 bytes.

1. Save a radio-off E200 burst, then `tx 2420 zero 15 0 80`, then `tx 2420 tone64 15 0 80`
   under narrow and wide. This last tone is +2 MHz at 80 MS/s. Start at gain 0, increase only
   as needed at fixed DAC amplitude. Keep ADC peaks <0.9 FS and integrated tone SNR >=30 dB.
   Ensure the duration fits both IDs (increase it for long callsigns). Exclude IDs. Require `ESP_OK`, filter matches/retained true, gain slot 0. At +2 MHz,
   wide image and carrier must each worsen <=3 dB versus the new narrow baseline; image
   <=-30 dBc and carrier <=-35 dBc. Prior OTA -35/-45 dBc is context, not a guaranteed floor.
2. `filter narrow`, then `tx 2420 sweep 60 0 80`; repeat with `filter wide`. Sweep payload:
   +1,+2,...,+20,-1,-2,...,-20 MHz, 250 ms each, repeats every 10 s. Complex tone amplitude
   64 counts, rounded per branch. CW ID replaces start/end payload; `seconds` includes IDs,
   so require enough interior payload for a full cycle. There is no off-box auto-key helper.
3. While the separate board command is running, collect finite E200 bursts at 61.44 MS/s,
   56 MHz requested RF bandwidth, center 2420 MHz. Existing `host/sdr.py` verifies all eight
   RX/TX-LO settings, scan layout, file size and saves a sidecar/hash. Configure once and
   call `SDR.capture` repeatedly; do not reconfigure per burst. Keep files from each filter
   in separate directories. Avoid the IDs, retain >=3 stationary bursts per frequency.
   Bursts crossing a 250 ms boundary are discarded. If the slim run misses points, report
   incomplete coverage and collect those points before claiming bandwidth.

Example receiver-only capture, while the coordinator's keyed command is already in payload:

```sh
python3 host/sdr.py notes/wide-01.iq --rate 61440000 --bandwidth 56000000 \
  --lo 2420000000 --rx-gain 0 --samples 4194304
python3 host/filter_response.py notes/wide/*.iq > notes/wide-response.json
```

For repeated buffers, `host.sdr.SDR(Settings(rate_hz=61440000, bandwidth_hz=56000000,
lo_hz=2420000000, gain_db=0))`, `configure()` once, then `capture(unique_path,4194304)`.
Pause irregularly by 0..40 ms between bursts to avoid repeatedly sampling the same sweep phase.
Use `from sdr import SDR, Settings` with `PYTHONPATH=host`. Save receiver settings unchanged
between narrow and wide. The analyzer opens files only. It rejects clipping, low SNR,
frequency transitions, missing reference points and insufficient three-burst coverage;
it does not distinguish a CW ID from a +2 MHz sweep tone. Supply only payload captures.
Its JSON gives line power, image, carrier, integration width and frequency coverage.
Lines below three times the estimated integrated noise are reported as upper limits
(`image_upper_limit` / `carrier_upper_limit`). Do not claim more rejection than those limits.

Compare `D(f) = Pwide_dBFS(f) - Pnarrow_dBFS(f)` at the **same RF frequency and receiver
settings**. This cancels a stable receive/path response and the DAC hold response.
Also retain absolute line powers: normalization must not hide gain collapse.
Normalize each mode to the mean dB power at +/-1 MHz. Proposed adoption gates:

- Wide response over every 1 MHz point through +/-18 MHz has max-min <=3 dB, both sides.
  Widening improvement at +/-18 MHz versus narrow is >=6 dB relative to +/-1 MHz.
  Confirm low-frequency absolute level changes <=3 dB. These are predeclared targets,
  not measurements. 40 MHz is a separate target: the same flatness gate through +/-20 MHz.
- Repeat at RX gain 10 dB lower (if SNR permits) and RX LO shifted +/-6 MHz, keeping TX
  fixed. Analyze with `--lo-offset 6000000` or `-6000000`. Normalized response should agree
  within 1 dB; otherwise receiver overload/response is unresolved. A sweep tone near RX DC
  is unusable at that LO; fill that point with the opposite LO shift. Keep AGC disabled and
  record DC/quadrature tracking settings. Known-flat source calibration is needed before
  attributing an absolute response entirely to TX. A same-RF A/B ratio remains useful.
- If full wide fails, run `filter analog` with identical settings. Adopt it only if it passes
  width and image/carrier gates. The branch's provisional auto choice must be revised to
  the measured winner before adoption. If neither passes, leave the wide requirement open;
  do not sweep TX capacitor codes or claim 36/40 MHz RF performance without evidence.
- Restore `filter auto`. Key `dvb 2420 11 26m667 30 0 128` and a narrow regression at the
  coordinator's chosen usable gain. Require zero underruns, late fills and fatal misses,
  positive slack, filter state as predicted (36 MHz selects wide), and all heap/deadline
  gates already used for this mode. RF LeanDVB decode is useful additional evidence if the
  receiver is qualified; USB bit exactness alone establishes no RF decode claim.

The quick board regression and narrow/wide RF screen can fit about five minutes. Full
three-burst coverage, shifted-LO repeats and calibrated absolute flatness are qualification
work and may need another session. Do not reduce the evidence standard to meet a time limit.
