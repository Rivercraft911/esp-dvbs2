"""Independent APSK formulas, EN 302 307-1 V1.4.1, 5.3.3/5.4.3/5.4.4/5.5.

Exact local standard text lines and independent digest provenance are recorded in
results/2026-10-16-apsk-decode/README.md. No external encoder code is used here.
Run: python3 tests/test_dvbs2_apsk.py (numpy, no hardware or receiver required).
"""
import hashlib
import json
from pathlib import Path
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "host"))
import dvbs2 as d
from test_dvbs2_8psk import header_complex, rotations
from test_dvbs2 import ts_input, digest

# Independent transcriptions of Tables 9/10, rather than using the encoder constants.
GAMMAS = {"16apsk": {"2/3": (3.15,), "3/4": (2.85,), "4/5": (2.75,),
                     "5/6": (2.70,), "8/9": (2.60,), "9/10": (2.57,)},
          "32apsk": {"3/4": (2.84, 5.27), "4/5": (2.72, 4.87), "5/6": (2.64, 4.64),
                     "8/9": (2.54, 4.33), "9/10": (2.53, 4.30)}}
MODES = [(m, r) for m, rates in GAMMAS.items() for r in rates]


def point(label, rate, modulation):
    """Figures 11/12 via bit fields and quadrant formulas, without ring label tables.

    b0 is the least significant bit. Non-outer 32APSK points and all 16APSK points
    reflect the first-quadrant coordinate in I and Q. The 32APSK outer ring walks
    alternating quadrants in reverse angular order, including the axis points.
    """
    b0, b1, b2, b3, b4 = [(label >> i) & 1 for i in range(5)]
    if modulation == "16apsk":
        gamma, = GAMMAS[modulation][rate]
        inner = b3 and b2
        radius = np.sqrt(4 / (1 + 3 * gamma**2)) * (1 if inner else gamma)
        angle = np.pi / 4 if inner else np.pi / 4 + (b3 - b2) * np.pi / 6
        return radius * ((1 - 2 * b1) * np.cos(angle) +
                         1j * (1 - 2 * b0) * np.sin(angle))
    g1, g2 = GAMMAS[modulation][rate]
    r1 = np.sqrt(8 / (1 + 3 * g1**2 + 4 * g2**2))
    if b3:
        quadrant = (3 if b1 else 0) if not b2 else (2 if b1 else 1)
        step = 2 * b0 + 1 - b4
        angle = quadrant * np.pi / 2 + (3 - step if quadrant % 2 else step) * np.pi / 8
        return r1 * g2 * np.exp(1j * angle)
    inner = b4 and b0
    radius = r1 * (1 if inner else g1)
    angle = np.pi / 4 if inner else np.pi / 4 + (b0 - b4) * np.pi / 6
    return radius * ((1 - 2 * b2) * np.cos(angle) +
                     1j * (1 - 2 * b1) * np.sin(angle))


def indices(width):
    return np.array([j + k * (64800 // width)
                     for j in range(64800 // width) for k in range(width)])


def test_interleaver_and_mapping():
    for modulation, rate in MODES:
        width = int(modulation[:2]).bit_length() - 1
        labels = np.arange(64800)
        assert np.array_equal(d.bit_interleave(labels, rate, modulation), indices(width))
        bits = np.array([[int(c) for c in f"{i:0{width}b}"] for i in range(1 << width)],
                        np.uint8).ravel()
        got = d.apsk_map(bits, rate, modulation)
        want = np.array([point(i, rate, modulation) for i in range(1 << width)])
        assert np.max(np.abs(got - want)) < 2e-15
        assert abs(np.mean(np.abs(got)**2) - 1) < 5e-16
        radii, counts = np.unique(np.round(np.abs(got), 12), return_counts=True)
        assert counts.tolist() == ([4, 12] if width == 4 else [4, 12, 16])
        assert np.allclose(radii[1:] / radii[0], GAMMAS[modulation][rate], atol=1e-10)
    assert [d.modcod(r, "16apsk") for r in GAMMAS["16apsk"]] == list(range(18, 24))
    assert [d.modcod(r, "32apsk") for r in GAMMAS["32apsk"]] == list(range(24, 29))
    print("APSK: every interleaver address and binary label, all 11 rates, radii and mean E=1")


def test_frames():
    rng = np.random.default_rng(20261016)
    for modulation, rate in MODES:
        width = 4 if modulation == "16apsk" else 5
        for pilots in (False, True):
            for n in (0, 262141):
                fec = rng.integers(0, 2, 64800, dtype=np.uint8)
                bits = fec[indices(width)].reshape(-1, width)
                labels = [int("".join(map(str, row)), 2) for row in bits]
                mapped = np.array([point(i, rate, modulation) for i in labels])
                # Build slot/pilot sequence by concatenation, independently of offset indexing.
                slots, chunks = len(mapped) // 90, []
                for s in range(slots):
                    chunks.append(mapped[90 * s:90 * (s + 1)])
                    if pilots and (s + 1) % 16 == 0 and s + 1 < slots:
                        chunks.append(np.full(36, (1 + 1j) / np.sqrt(2)))
                body = np.concatenate(chunks)
                base = 18 if width == 4 else 24
                mc = base + list(GAMMAS[modulation]).index(rate)
                want = np.concatenate([header_complex(mc, False, pilots),
                                       body * rotations(len(body), n)])
                got = d.plframe_symbols(fec, rate, pilots=pilots, n=n, modulation=modulation)
                assert len(got) == d.plframe_length(False, pilots, modulation) == len(want)
                assert np.max(np.abs(got - want)) < 3e-15
    assert d.plframe_length(False, True, "16apsk") == 16686
    assert d.plframe_length(False, False, "16apsk") == 16290
    assert d.plframe_length(False, True, "32apsk") == 13338
    assert d.plframe_length(False, False, "32apsk") == 13050
    print("APSK: 44 PL frames, independent header/Gold formulas, all pilot boundaries, n=0/262141")


def test_gcs_and_compatibility():
    rng = np.random.default_rng(20261016)
    for modulation, rate in MODES:
        capacity = d.code(rate).kbch - 80
        data = rng.integers(0, 2, capacity + 13, dtype=np.uint8)
        stream = d.transmit_symbols(data, rate, modulation=modulation)
        width = 4 if modulation == "16apsk" else 5
        length = d.plframe_length(False, True, modulation)
        points = np.array([point(i, rate, modulation) for i in range(1 << width)])
        recovered = []
        for start in range(0, len(stream), length):
            body = stream[start + 90:start + length] / rotations(length - 90, 0)
            pos = np.arange(64800 // width)
            data_symbols = body[pos + 36 * (pos // 1440)]
            labels = np.argmin(np.abs(data_symbols[:, None] - points), axis=1)
            bits = ((labels[:, None] >> np.arange(width - 1, -1, -1)) & 1).astype(np.uint8)
            fec = np.empty(64800, np.uint8)
            fec[indices(width)] = bits.ravel()
            bbf = d.bb_scramble(fec[:d.code(rate).kbch])
            assert d.crc8(bbf[:80]) == 0
            dfl = int("".join(map(str, bbf[32:48])), 2)
            recovered.append(bbf[80:80 + dfl])
            assert not bbf[80 + dfl:].any()
        assert [len(f) for f in recovered] == [capacity, 13]
        assert np.array_equal(np.concatenate(recovered), data)
    for modulation in ("qpsk", "8psk"):
        fec = rng.integers(0, 2, 64800, dtype=np.uint8)
        assert np.array_equal(d.plframe_symbols(fec, "2/3", modulation=modulation),
                              d.symbols_from_phases(d.plframe_phases(fec, "2/3",
                                                                      modulation=modulation)))
    for function, args, kwargs in (
            (d.modcod, ("1/2", "16apsk"), {}), (d.modcod, ("2/3", "32apsk"), {}),
            (d.plframe_length, (True, True, "16apsk"), {}),
            (d.transmit_symbols, ([], "3/4"), {"short": True, "modulation": "32apsk"}),
            (d.transmit_phases, ([], "2/3"), {"modulation": "16apsk"}),
            (d.plframe_phases, (fec, "3/4"), {"modulation": "32apsk"})):
        try:
            function(*args, **kwargs)
        except ValueError:
            pass
        else:
            raise AssertionError("unsupported mode accepted")
    print("APSK: 11 two-frame GCS reversals, partial 13-bit DFL, PSK compatibility, invalid modes")


def test_crosscheck():
    records = json.loads((Path(__file__).parent / "data/apsk_digests.json").read_text())
    packets = ts_input(200)
    for modulation, rate in MODES:
        width = 4 if modulation == "16apsk" else 5
        bb = d.ts_bbframes(packets, rate)[:3]
        scr = [d.bb_scramble(b) for b in bb]
        bch = [d.bch_encode(b, rate) for b in scr]
        fec = [d.ldpc_encode_groups(b, rate) for b in bch]
        stages = {k: digest(np.concatenate(v)) for k, v in
                  dict(bb=bb, scr=scr, bch=bch, fec=fec).items()}
        inter = [d.bit_interleave(f, rate, modulation) for f in fec]
        labels = np.concatenate([b.reshape(-1, width) @ (1 << np.arange(width - 1, -1, -1))
                                 for b in inter]).astype(np.uint8)
        stages["inter"] = hashlib.sha256(labels.tobytes()).hexdigest()[:32]
        def complex_digest(x):
            grid = np.rint(np.stack([x.real, x.imag], axis=-1) * 10000).astype("<i4")
            return hashlib.sha256(grid.tobytes()).hexdigest()[:32]
        mapped = np.concatenate([d.apsk_map(b, rate, modulation) for b in inter])
        for pilots in (False, True):
            framed = np.concatenate([d.plframe_symbols(f, rate, pilots=pilots,
                                                       modulation=modulation) for f in fec])
            for implementation in ("grdtv", "leansdr"):
                rec = records[f"{implementation} {modulation} {rate} {int(pilots)}"]
                for stage, want in stages.items():
                    assert rec[stage] == want, (implementation, modulation, rate, stage)
                if implementation == "grdtv":
                    assert rec["map_q1e4"] == complex_digest(mapped)
                    assert rec["pl_q1e4"] == complex_digest(framed)
    assert len(records) == 44
    print("APSK cross-check: gr-dtv/LeanSDR BB, scrambler, BCH, LDPC, interleaver digests; "
          "gr-dtv mapped/full PL coordinates rounded to 1e-4; 11 modes x pilots on/off x 3 frames")


if __name__ == "__main__":
    test_interleaver_and_mapping()
    test_frames()
    test_gcs_and_compatibility()
    test_crosscheck()
