"""Regenerate dvbs2_tables.py from the ETSI PDF: python3 host/gen_tables.py PATH/TO/standard.pdf

Source: ETSI EN 302 307-1 V1.4.1 (2014-11), free from
https://www.etsi.org/deliver/etsi_en/302300_302399/30230701/01.04.01_60/en_30230701v010401p.pdf
(sha256 checked below; the PDF is not stored in this repository), converted with poppler
`pdftotext -raw` (reading order, which handles the multi-column
pages of Annex B/C). Extracted: Tables 5a/5b (Kbch, Nbch, t), 6a/6b (BCH polynomials), 7a/7b (q)
and Annex B/C (LDPC parity accumulator addresses).

LDPC row boundaries. The PDF sets the address tables as flowed text: long rows wrap, and in places
two rows run together in one paragraph (e.g. Table B.2 rows 2-3), so the row split cannot be
recovered from the text. The values are taken in order from the PDF and split with ROWS below
(entries per row, number of rows). ROWS equals the row structure of gr-dtv (gnuradio aee9fd3,
dvb_ldpc_bb_impl.cc) and xdsopl/LDPC (32357d8, dvb_s2_tables.hh DEG/LEN), and
the RP2350 benchmark's crosscheck confirmed the full tables match both row by row. It also agrees with
the row lengths the standard prints: 13 addresses for i0 at rate 2/3 (5.3.2.1) and 8 at rate 1/2
(Annex B example for Table B.4).
"""
import hashlib
import pathlib
import re
import subprocess

HERE = pathlib.Path(__file__).resolve().parent
PDF_SHA256 = "19077f80420eb17519fcf6f83e2847d7a877940b134c5306814b8c6f60d8475d"
RATES = ("1/4", "1/3", "2/5", "1/2", "3/5", "2/3", "3/4", "4/5", "5/6", "8/9", "9/10")

# (entries per row, number of rows), in table order; see the module docstring.
ROWS = {
    ("normal", "1/4"): [(12, 15), (3, 30)], ("normal", "1/3"): [(12, 20), (3, 40)],
    ("normal", "2/5"): [(12, 24), (3, 48)], ("normal", "1/2"): [(8, 36), (3, 54)],
    ("normal", "3/5"): [(12, 36), (3, 72)], ("normal", "2/3"): [(13, 12), (3, 108)],
    ("normal", "3/4"): [(12, 15), (3, 120)], ("normal", "4/5"): [(11, 18), (3, 126)],
    ("normal", "5/6"): [(13, 15), (3, 135)], ("normal", "8/9"): [(4, 20), (3, 140)],
    ("normal", "9/10"): [(4, 18), (3, 144)],
    ("short", "1/4"): [(12, 4), (3, 5)], ("short", "1/3"): [(12, 5), (3, 10)],
    ("short", "2/5"): [(12, 6), (3, 12)], ("short", "1/2"): [(8, 5), (3, 15)],
    ("short", "3/5"): [(12, 9), (3, 18)], ("short", "2/3"): [(13, 3), (3, 27)],
    ("short", "3/4"): [(12, 1), (3, 32)], ("short", "4/5"): [(3, 35)],
    ("short", "5/6"): [(13, 1), (3, 36)], ("short", "8/9"): [(4, 5), (3, 35)],
}


def pdf_text(pdf):
    data = pathlib.Path(pdf).read_bytes()
    assert hashlib.sha256(data).hexdigest() == PDF_SHA256, "unexpected PDF revision"
    out = subprocess.run(["pdftotext", "-raw", str(pdf), "-"],
                         check=True, capture_output=True, text=True)
    return out.stdout.replace("\x0c", "\n")    # a form feed glues a table's last line to the footer


def num(s):                                     # "16 008" -> 16008 (thin-space separator)
    return int(s.replace(" ", ""))


def section(t, start, end):                     # text from `start` to the next `end` (skips TOC)
    a = t.index(start)
    return t[a:t.index(end, a)]


def parse(t):
    n = r"(\d{1,2} \d{3}|\d+)"
    t5a = section(t, "Table 5a: Coding", "Table 5b: Coding")
    t5b = section(t, "Table 5b: Coding", "5.3.1 Outer encoding")
    bch = {"normal": {m[0]: (num(m[1]), num(m[2]), int(m[3]))
                      for m in re.findall(rf"^(\d+/\d+) {n} {n} (\d+) 64 800", t5a, re.M)},
           "short": {m[0]: (num(m[1]), num(m[2]), int(m[3]))
                     for m in re.findall(rf"^(\d+/\d+) {n} {n} (\d+) \d+/\d+ 16 200", t5b, re.M)}}
    t7a = section(t, "Table 7a: q values", "5.3.2.2")
    t7b = section(t, "Table 7b: q values", "5.3.3 Bit Interleaver")
    q = {f: {m[0]: int(m[1]) for m in re.findall(r"^(\d+/\d+) (\d+)", s, re.M)}
         for f, s in (("normal", t7a), ("short", t7b))}
    # Tables 6a/6b: "g3(x) 1+x2+x3+..." (superscripts flattened: x2 means x^2, bare x means x^1)
    t6 = section(t, "Table 6a: BCH polynomials", "BCH encoding of information bits")
    polys = []
    for m in re.finditer(r"^g(\d+)\(x\) ([0-9x+]+)", t6, re.M):
        polys.append(tuple(0 if tm == "1" else int(tm[1:] or 1) for tm in m[2].split("+")))
    bpoly = {"normal": polys[:12], "short": polys[12:24]}
    assert len(polys) == 24 and bpoly["normal"][0][-1] == 16 and bpoly["short"][0][-1] == 14
    # Annex B/C, from the first table heading up to Annex D
    body = section(t, "Example of interpretation of table B.4", "Annex D (normative)")
    heads = list(re.finditer(
        r"Table ([BC])\.(\d+): Rate (\d+/\d+) \(nldpc = (64 800|16 200)\)", body))
    ldpc = {}
    for i, h in enumerate(heads):
        end = heads[i + 1].start() if i + 1 < len(heads) else len(body)
        vals = [int(x) for line in body[h.end():end].split("\n")
                if (tok := line.split()) and all(x.isdigit() for x in tok) for x in tok]
        key = ("normal" if h[1] == "B" else "short", h[3])
        rows, p = [], 0
        for deg, count in ROWS[key]:
            for _ in range(count):
                rows.append(tuple(vals[p:p + deg]))
                p += deg
        assert p == len(vals), (key, p, len(vals))
        assert len(rows) * 360 == bch[key[0]][key[1]][1], key   # one row per 360 info bits
        ldpc[key] = rows
    assert len(ldpc) == 21
    return bch, q, bpoly, ldpc


def write(bch, q, bpoly, ldpc):
    o = ['"""DVB-S2 code tables, generated by gen_tables.py from ETSI EN 302 307-1 V1.4.1 '
         '(2014-11),',
         f'en_30230701v010401p.pdf (sha256 {PDF_SHA256[:16]}...). Do not edit by '
         'hand."""',
         "", "# Tables 5a/5b: rate -> (Kbch, Nbch = kldpc, t)", "BCH_PARAMS = {"]
    for f in ("normal", "short"):
        o += [f"    {f!r}: {{"]
        o += [f"        {r!r}: {bch[f][r]}," for r in RATES if r in bch[f]]
        o += ["    },"]
    o += ["}", "", "# Tables 7a/7b: rate -> q = (nldpc - kldpc)/360", "Q = {"]
    for f in ("normal", "short"):
        o.append(f"    {f!r}: {{" + ", ".join(f"{r!r}: {q[f][r]}" for r in RATES if r in q[f])
                 + "},")
    o += ["}", "", "# Tables 6a/6b: exponents of g1(x) .. g12(x)", "BCH_POLYS = {"]
    for f in ("normal", "short"):
        o.append(f"    {f!r}: [")
        o += [f"        {p}," for p in bpoly[f]]
        o.append("    ],")
    o += ["}", "",
          "# Annex B (normal) / Annex C (short): parity accumulator addresses x, one row per group",
          "# of 360 information bits (row g serves i_{360g} .. i_{360g+359}).", "LDPC = {"]
    for key, rows in ldpc.items():
        o.append(f"    {key}: (")
        o += ["        (" + ", ".join(map(str, r)) + ",)," for r in rows]
        o.append("    ),")
    o += ["}", ""]
    (HERE / "dvbs2_tables.py").write_text("\n".join(o))


if __name__ == "__main__":
    import sys
    write(*parse(pdf_text(sys.argv[1])))
    print("wrote", HERE / "dvbs2_tables.py")
