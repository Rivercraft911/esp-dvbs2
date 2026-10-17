# Device wrapper and default replay, host only

QPSK 3/4 at 10 Msym/s is now the demo default. Clean reference-file playback
ran for 18.165 s at 20 MS/s, with 5,455 good FEC frames, zero failures and
236 decoded video pictures. Final input delivery was 19.998 MS/s with 1.56 ms
input-clock deficit. This is a backend check, not an SDR, RF, browser or camera
qualification. No serial port, SDR or lab network device was accessed.

`runs.jsonl` retains the command, final snapshot, source/receiver/FFmpeg hashes,
fixture metadata and software versions. The run used uncommitted code on
`0aabe1c`; the recorded source bytes match the subsequent `d27a919` commit.
The source was the default two-second 960 × 480, 15 frame/s H.264 test card.
Repeated MPEG timestamps/TS counters still cause replay-join warnings and lost
video pictures: 236/18.165 = 13.0 pictures/s is not uninterrupted 15 frame/s.

The new SoapySDR layer is tested with a fake driver only. Tests cover B200 and
E200 selection, RX-only setup, rate/frequency/filter/gain readbacks, explicit
settings and serial selection, short reads, timeouts, overflows, cleanup and
error propagation into the dashboard. B200/SoapyUHD and E200/SoapyPlutoSDR
initialization and live delivery remain for the coordinator. E200 also retains
the existing libiio 0.25 path. [Commands and bandwidth derivations](../../docs/demo-rx.md).

Headless Chrome aborted with SIGABRT under the process sandbox. The fallback
static check executes the actual HTML script in jsdom with intercepted input
and real CPU 2D canvases. It verifies spectrum/waterfall pixels, DOM values,
CSS parsing, video API calls, panorama controls, stale/error states and flat
video without WebGL. It caught two defects: no picture without WebGL, and
source errors hidden in that mode. Both are fixed. GPU shader output,
responsive layout and actual fullscreen remain unverified. No whole-dashboard
screenshot is claimed. The static test is now in CI.

An initial integration run with a one-second QPSK loop failed its 300 ms startup
lag assertion at 354 ms. The integration test now uses the actual default
two-second fixture and also checks final lag. The full run passes, including
decoded video PID payload comparisons, JSON/JPEG endpoints and paced looping.
This does not establish smooth playback for arbitrary short loop lengths.

Verification: 14 receiver tests including reference-video integration, 9 fake
device tests, the static render check in both WebGL-double and no-WebGL modes,
and existing `test_sdr.py`, `test_iq.py` and `test_decode.py`. JavaScript syntax
and `git diff --check` pass. `origin/main` at `da97362` was merged in `0aabe1c`;
the README conflict was resolved around main's cleaned version.
