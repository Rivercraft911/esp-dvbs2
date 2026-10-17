# next4 offline integration

All 30 host test scripts pass. All four QEMU apps pass: encoder, shaper/sample
glue, radio dry run, and APSK. ESP-IDF v6.1 builds the firmware and all harnesses
without compiler warnings. No hardware serial port was opened and nothing was
flashed or transmitted.

The firmware source revision is `c75812a`, containing the two merges
`a7e55f2` / `f00537a` and the short-layout Wi-Fi allocation probe. The initial
build's app version is `f00537a-dirty`; its tracked firmware contents equal
`c75812a`. Documentation and the coordinator script are committed afterward.
The main comparison is a `git archive 32ea843 firmware` snapshot built with
the same generated sdkconfig and toolchain.

`runs.jsonl` contains the host exits, QEMU counts, linked sizes and copied
summaries of the supplied prior board records. Rows tagged `prior_board` are
measurements of the named input firmware, not measurements of this integration.
Each identifies its source file and SHA-256. Other rows are offline results.
Full build/test/emulator logs and the script's mocked transport fixtures are in
the worktree's ignored `notes/` directory.

The host stream pins all eight legacy and both new dump CRCs and checks every
sample against the plain encoder oracle. It covers complete 1,365-bank and
819-bank wrap periods, allocation failures and cleanup. QEMU checks 384 packed
encoder frames with both mappers and changing workspace tails, 140 reference
FEC CRCs and 14 PIE checks. Shaper checks include 324 legacy cases, 162 short-pulse
cases, 2,560 sample-glue cases, 26,114 unpack cases and 864 word-copy cases.
APSK completes 1,760 checks with zero failures. These establish correctness,
not hardware timing.

Static internal capacity improves by 2,576 B versus main. `20ms6` also removes
an 11,120 B heap frame, saving 13,696 B before allocator effects. At 8 sps the
new 2,058 B phase window reduces the static benefit to 518 B. The sparse mapper
adds no allocation. See [the derivation and fixed board gates](../../docs/next4.md).

The [coordinator script](../../host/next4-board.sh) passes `bash -n` and a mocked
21-command run, including ten saved binaries with their expected CRCs. Separate
CRC, late-switch and low-heap injections each stop the script. The mock only
tests command routing, transport checks and gates; it is not a waveform result.
The script has not been run on the board.
