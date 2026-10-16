"""BBFRAME/status checks, then optionally an independent IQ receiver loopback.

python3 tests/test_decode.py                   # parser and failure checks
python3 tests/test_decode.py --loopback        # needs the local LeanDVB build
The integration test checks complete, nonrepeating application files between guard
frames. It cannot pass by recovering only a repeated or partial payload.
"""
import argparse
import json
from pathlib import Path
import sys
import tempfile

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "host"))
import decode as rx
import dvbs2 as tx


def rejected(function, *args, **kwargs):
    try:
        function(*args, **kwargs)
    except ValueError:
        return
    raise AssertionError("invalid input accepted")


def test_parsers():
    payload = bytes(range(250))
    header = np.packbits(tx.bbheader(tx.matype1(tx.GCS, 0.20), len(payload) * 8)).tobytes()
    raw = header + payload
    frames = rx.parse_bbframes(raw + raw)
    assert len(frames) == 2 and frames[0]["payload"] == payload
    assert frames[0]["rolloff_code"] == 2
    rejected(rx.parse_bbframes, raw[:-1])
    rejected(rx.parse_bbframes, raw + b"\x70")
    rejected(rx.parse_bbframes, bytes([raw[0] ^ 1]) + raw[1:])
    rejected(rx.parse_bbframes,
             np.packbits(tx.bbheader(tx.matype1(tx.GCS), 7)).tobytes() + b"\x00")
    rejected(rx.parse_bbframes,
             np.packbits(tx.bbheader(tx.matype1(tx.GCS), 0, upl=1504)).tobytes())
    status = ("LOCK 0\nFRAMELOCK 0\nFRAMELOCK 1\n"
              "S2FEC modcod=6 short=0 pilots=1 ldpc_flips=3 "
              "ldpc_checks_before=10 ldpc_checks_after=2 bch_errors=1\n"
              "S2FEC modcod=6 short=0 pilots=1 ldpc_flips=100 "
              "ldpc_checks_before=90 ldpc_checks_after=20 bch_errors=-1\n")
    report = rx.summarize(status, frames, b"prefix" + payload + b"suffix", payload)
    assert report["frame_lock"] and report["lock_at_end"] and report["payload_match"]
    assert report["fec_frames"] == 2 and report["bch_failed_frames"] == 1
    assert report["ldpc_residual_frames"] == 2 and report["bch_corrected_bits"] == 1
    assert report["ldpc_bit_flips"] == 103 and report["match_offset_bytes"] == 6
    assert not rx.summarize(status, frames, payload[:-1], payload)["payload_match"]
    # An empty BBFRAME (DFL = 0), the firmware's filler when its encoder is late.
    empty = np.packbits(tx.bbheader(tx.matype1(tx.GCS), 0)).tobytes()
    mixed = rx.parse_bbframes(raw + empty + raw)
    assert [f["payload"] for f in mixed] == [payload, b"", payload]
    assert rx.summarize(status, mixed, payload * 2)["empty_bb_frames"] == 1
    assert not rx.summarize("LOCK 1\n", frames, payload)["frame_lock"]
    assert rx.summarize("", [], b"")["ldpc_checks_after"] is None
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "samples.cfile"
        path.write_bytes(b"\0" * 8)
        assert rx.capture_info(path, 4e6, 1e6)[1:] == (4e6, 1e6, 0.35)
        rejected(rx.capture_info, path, None, 1e6)
        rejected(rx.capture_info, path, 1e6, 1e6)
        path.write_bytes(b"\0")
        rejected(rx.capture_info, path, 4e6, 1e6)
        path = Path(tmp) / "test.sigmf-data"
        path.write_bytes(b"\0" * 8)
        metadata = {"global": {"core:datatype": "cf32_le", "core:sample_rate": 4e6,
                               "dvbs2:symbol_rate": 1e6, "dvbs2:rolloff": 0.20},
                    "captures": [{"core:sample_start": 0}]}
        meta = path.with_suffix(".sigmf-meta")
        meta.write_text(json.dumps(metadata))
        assert rx.capture_info(meta)[1:] == (4e6, 1e6, 0.20)
        rejected(rx.capture_info, meta, sample_rate=0)
        rejected(rx.capture_info, meta, symbol_rate=0)
        for key, value in (("core:num_channels", 2), ("core:dataset", "other-data"),
                           ("core:offset", 8), ("core:trailing_bytes", 8)):
            metadata["global"][key] = value
            meta.write_text(json.dumps(metadata))
            rejected(rx.capture_info, meta)
            del metadata["global"][key]
        for key, value in (("core:sample_start", 1), ("core:header_bytes", 4)):
            metadata["captures"] = [{key: value}]
            meta.write_text(json.dumps(metadata))
            rejected(rx.capture_info, meta)
    print("decode parsers: independent BBHEADERs, truncated/corrupt data, GCS lock, "
          "FEC failures, incomplete payload rejected, empty BBFRAMEs counted", flush=True)


def phase_stream(rate, modulation, alpha, seed=20261016):
    """Four training frames, three application fields (last partial), four tail frames."""
    rng = np.random.default_rng(seed)
    capacity = (tx.code(rate).kbch - 80) // 8
    expected = rng.integers(0, 256, 2 * capacity + 137, dtype=np.uint8).tobytes()
    guard = rng.integers(0, 256, capacity, dtype=np.uint8)
    guard_phases = tx.transmit_phases(np.unpackbits(guard), rate, ro=alpha,
                                     modulation=modulation)
    target = tx.transmit_phases(np.unpackbits(np.frombuffer(expected, np.uint8)),
                               rate, ro=alpha, modulation=modulation)
    phases = np.concatenate([np.tile(guard_phases, 4), target, np.tile(guard_phases, 4)])
    return phases, expected


def loopback(receiver=None, out=None):
    import iq
    modes = (("qpsk", "1/2"), ("qpsk", "2/3"), ("qpsk", "3/4"),
             ("8psk", "2/3"), ("8psk", "5/6"))
    records = []
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        for modulation, rate in modes:
            for alpha in (0.20, 0.35):
                phases, expected = phase_stream(rate, modulation, alpha)
                for rs in (1e6, 5e6):
                    # E200 host rates: 4 MS/s and 10 MS/s, respectively.
                    fs = 4e6 if rs == 1e6 else 10e6
                    for impaired in (False, True):
                        channel = ({"snr_db": 14, "frequency_offset": 0.01 * rs,
                                    "sample_rate_offset_ppm": 50, "seed": 20261016}
                                   if impaired else {})
                        samples = iq.shape(phases, rs, fs, alpha=alpha, **channel)
                        capture = tmp / "loop.sigmf-data"
                        iq.write_capture(capture, samples, fs, rs, alpha=alpha)
                        report, recovered = rx.decode(capture, receiver=receiver,
                                                       expected=expected, log=tmp / "receiver.log")
                        assert report["frame_lock"] and report["status_complete"], report
                        assert report["modcods"] == [tx.modcod(rate, modulation)], report
                        assert report["payload_match"], report
                        start = report["match_offset_bytes"]
                        assert recovered[start:start + len(expected)] == expected
                        if not impaired:
                            assert report["bch_failed_frames"] == 0, report
                            assert report["ldpc_checks_after"] == 0, report
                        record = {"modulation": modulation, "rate": rate, "alpha": alpha,
                                  "symbol_rate": rs, "sample_rate": fs, "impaired": impaired,
                                  "channel": channel,
                                  **{k: v for k, v in report.items() if k != "command"}}
                        records.append(record)
                        if out:
                            with Path(out).open("a") as destination:
                                destination.write(json.dumps(record, sort_keys=True) + "\n")
                        print(f"{modulation} {rate} alpha={alpha} {rs/1e6:g} Msym/s "
                              f"{'AWGN+CFO+SRO' if impaired else 'clean'}: "
                              f"{report['bb_frames']} BB frames, {len(expected)} exact bytes, "
                              f"{report['ldpc_bit_flips']} LDPC flips, "
                              f"{report['bch_failed_frames']} rejected FEC frames", flush=True)
    assert len(records) == 40
    print("independent LeanDVB loopback: all 40 cases passed", flush=True)
    test_capture_edges(receiver)


def test_capture_edges(receiver):
    """Exercise the optional hold, an arbitrary capture start, and TS deframing."""
    import iq
    phases, expected = phase_stream("5/6", "8psk", 0.35)
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "edge.cfile"
        for label, settings, crop in (
                ("DAC hold", {"dac_rate": 80e6, "snr_db": 18,
                              "frequency_offset": -50000,
                              "sample_rate_offset_ppm": -50, "seed": 20261016}, 0),
                ("mid-frame start", {}, int(0.73 * 2 * tx.plframe_length(False, True, "8psk")))):
            samples = iq.shape(phases, 5e6, 10e6, **settings)[crop:]
            iq.write_capture(path, samples, 10e6, 5e6)
            report, recovered = rx.decode(path, sample_rate=10e6, symbol_rate=5e6,
                                           receiver=receiver, expected=expected)
            assert report["payload_match"] and report["modcods"] == [15], report
            wrong = bytes([expected[0] ^ 1]) + expected[1:]
            assert not rx.summarize("", [], recovered, wrong)["payload_match"]
            print(f"8psk 5/6 {label}: {len(expected)} exact bytes; altered expected file fails",
                  flush=True)
        rng = np.random.default_rng(20261016)
        packets = rng.integers(0, 256, (800, 188), dtype=np.uint8)
        packets[:, 0] = 0x47
        packets[:, 1] &= 0x7f  # clear transport_error_indicator
        phases = np.concatenate([tx.plframe_phases(tx.fecframe(bbf, "2/3"), "2/3")
                                 for bbf in tx.ts_bbframes(packets, "2/3")])
        samples = iq.shape(phases, 1e6, 4e6)
        iq.write_capture(path, samples, 4e6, 1e6)
        expected_ts = packets[200:400].tobytes()
        report, recovered = rx.decode(path, sample_rate=4e6, symbol_rate=1e6,
                                       receiver=receiver, expected=expected_ts)
        assert report["stream"] == "ts" and report["payload_match"], report
        assert report["modcods"] == [6] and report["bch_failed_frames"] == 0, report
        assert not any(recovered[k + 1] & 0x80 for k in range(0, len(recovered), 188))
        print(f"TS: {len(recovered)//188} packets, 200 central packets "
              f"({len(expected_ts)} bytes) exact, no transport errors", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--loopback", action="store_true")
    parser.add_argument("--receiver", type=Path)
    parser.add_argument("--out", type=Path, help="append loopback JSONL records")
    args = parser.parse_args()
    test_parsers()
    if args.loopback:
        loopback(args.receiver, args.out)
