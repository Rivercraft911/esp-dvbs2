"""DVB-S2 reference transmitter: user bits -> BBFRAME -> FECFRAME -> PLFRAME symbols.

This is the readable model the firmware must match bit for bit. Standard: ETSI EN 302 307-1
V1.4.1 (2014-11); clause, table and figure numbers refer to it. Code tables: dvbs2_tables.py,
generated from the standard's PDF by gen_tables.py. Carried over from the RP2350 benchmark,
where tests/test_dvbs2.py checked it against gr-dtv and leansdr stage by stage.

Scope: QPSK (MODCOD 1-11) and 8PSK (12-17), normal (64 800 bit) and short (16 200 bit)
FECFRAMEs; 16APSK (18-23) and 32APSK (24-28), normal frames only (Table 12). Pilots on/off,
any PL scrambling code n (default 0). Not implemented:
dummy PLFRAMEs (5.5.1), ISSY and null-packet deletion (Annex D), multiple streams, ACM,
pulse shaping (5.6).

Conventions
* Bit arrays are numpy uint8 0/1 in transmission order; index 0 is the first bit sent (the
  standard's MSB).
* The QPSK API uses bit pairs (bI, bQ), I = (1 - 2 bI)/sqrt2, Q = (1 - 2 bQ)/sqrt2.
* The common QPSK/8PSK API uses a uint8 phase index p: exp(j p pi/4), p = 0..7. All data,
  pi/2-BPSK header and pilot symbols lie on these eight points, before and after PL scrambling.
  One phase byte per symbol also selects the pulse shaper's eight possible input symbols.
* plframe_symbols()/transmit_symbols() return complex symbols for all four modulations.
  APSK uses unit average constellation energy, one of the two scalings admitted by 5.4.3/4.

Exact standard text line citations and normalization derivations: results/2026-10-16-apsk-decode/README.md.

Mode adaptation for file transfer: Generic Continuous Stream (GCS). MATYPE-1 = 01 1 1 0 0 RO
(Table 3: TS/GS = 01 generic continuous, SIS, CCM, ISSYI off, NPD off, roll-off), MATYPE-2 = 0
(reserved for a single stream), UPL = 0 (continuous, 5.1.1), DFL = user bits in this frame,
SYNC = 0x00 (for GCS 00-B8 are reserved for transport-layer protocol signalling, B9-FF user
private; 5.1.6), SYNCD = 0 (reserved for GCS). Why: file framing, sequence numbers and a strong
CRC belong to the application layer. In GCS the stream passes the CRC-8 stage unmodified
(5.1.4) and the slicer keeps no packet state; DFL marks the valid bits, so the last frame of a
file is simply short and zero-padded (5.2.1), and application packets aligned to BBFRAMEs are
lost only with their own frame. Packetized mode (TS/GS = 00) would add one CRC-8 per packet and
split packets across frames. ts_bbframes() implements the single-TS mode of Table 4 only because
the independent implementations used for the cross-check take TS input.
"""
from collections import namedtuple
from functools import lru_cache
import pathlib
import sys

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from dvbs2_tables import BCH_PARAMS, BCH_POLYS, LDPC, Q  # noqa: E402

RATES = ("1/4", "1/3", "2/5", "1/2", "3/5", "2/3", "3/4", "4/5", "5/6", "8/9", "9/10")
PSK8_RATES = ("3/5", "2/3", "3/4", "5/6", "8/9", "9/10")
APSK16_RATES = ("2/3", "3/4", "4/5", "5/6", "8/9", "9/10")
APSK32_RATES = ("3/4", "4/5", "5/6", "8/9", "9/10")
BITS_PER_SYMBOL = {"qpsk": 2, "8psk": 3, "16apsk": 4, "32apsk": 5}
# 5.4.3 Table 9; 5.4.4 Table 10, dimensionless radius ratios, in rate order above.
APSK16_GAMMA = (3.15, 2.85, 2.75, 2.70, 2.60, 2.57)
APSK32_GAMMA = ((2.84, 5.27), (2.72, 4.87), (2.64, 4.64), (2.54, 4.33), (2.53, 4.30))
Code = namedtuple("Code", "rate short kbch nbch t n q modcod rows")


@lru_cache(maxsize=None)
def code(rate, short=False):
    """Tables 5a/5b (Kbch, Nbch = kldpc, t), 7a/7b (q), 12 (QPSK MODCOD 1..11), Annex B/C."""
    f = "short" if short else "normal"
    kbch, nbch, t = BCH_PARAMS[f][rate]
    rows = tuple(np.array(r) for r in LDPC[(f, rate)])
    n = 16200 if short else 64800
    return Code(rate, short, kbch, nbch, t, n, Q[f][rate], RATES.index(rate) + 1, rows)


def modcod(rate, modulation="qpsk"):
    """Table 12: QPSK 1..11, 8PSK 12..17, 16APSK 18..23, 32APSK 24..28."""
    if modulation == "qpsk":
        return RATES.index(rate) + 1
    if modulation == "8psk":
        return PSK8_RATES.index(rate) + 12
    if modulation == "16apsk":
        return APSK16_RATES.index(rate) + 18
    if modulation == "32apsk":
        return APSK32_RATES.index(rate) + 24
    raise ValueError(f"unsupported modulation: {modulation}")


def int_bits(v, n):
    """n-bit integer v as a bit array, MSB first."""
    return np.array([(v >> (n - 1 - k)) & 1 for k in range(n)], np.uint8)


# ---------------------------------------------------------- mode and stream adaptation (5.1, 5.2)

def crc8(bits):
    """CRC-8 of 5.1.4 (Figure 2): remainder[X^8 u(X) : g(X)], g = X^8+X^7+X^6+X^4+X^2+1,
    register zeroed, input MSB first. Returned as an int, transmitted MSB first."""
    c = 0
    for b in np.asarray(bits).tolist():
        c = ((c << 1) & 0xFF) ^ (0xD5 if (c >> 7) ^ b else 0)
    return c


GCS, TS = 0b01, 0b11                         # Table 3 TS/GS field
RO = {0.35: 0b00, 0.25: 0b01, 0.20: 0b10}    # Table 3 RO field


def matype1(ts_gs, ro=0.35, sis=1, ccm=1, issyi=0, npd=0):
    """MATYPE-1 (5.1.6, Table 3): TS/GS[7:6] SIS/MIS[5] CCM/ACM[4] ISSYI[3] NPD[2] RO[1:0]."""
    return ts_gs << 6 | sis << 5 | ccm << 4 | issyi << 3 | npd << 2 | RO[ro]


def bbheader(mt1, dfl, upl=0, sync=0, syncd=0, mt2=0):
    """BBHEADER, 80 bits (5.1.6, Figure 3): MATYPE-1, MATYPE-2, UPL(16), DFL(16), SYNC(8),
    SYNCD(16), then the CRC-8 of those 72 bits (Figure 2, switch in A for 72 bits). Sent from
    the MSB of TS/GS."""
    b = int_bits(mt1 << 64 | mt2 << 56 | upl << 40 | dfl << 24 | sync << 16 | syncd, 72)
    return np.concatenate([b, int_bits(crc8(b), 8)])


def bbframe(data, rate, short=False, ro=0.35, sync=0):
    """GCS BBFRAME before scrambling (Kbch bits): BBHEADER, DATA FIELD of DFL = len(data)
    <= Kbch - 80 bits (5.1.5), then Kbch - DFL - 80 zero bits of padding (5.2.1)."""
    c = code(rate, short)
    data = np.asarray(data, np.uint8)
    assert len(data) <= c.kbch - 80
    h = bbheader(matype1(GCS, ro), len(data), sync=sync)
    return np.concatenate([h, data, np.zeros(c.kbch - 80 - len(data), np.uint8)])


def ts_bbframes(packets, rate, short=False, ro=0.35):
    """Single-TS CCM BBFRAMEs before scrambling (Table 4), for the cross-check only. The sync byte
    of each 188-byte packet is replaced by the CRC-8 of the previous packet's 187 bytes (5.1.4;
    undefined for the first packet, gr-dtv and leansdr both use 0). DATA FIELDs of Kbch - 80 bits
    are cut across packet boundaries ("Break"); SYNCD = bits from the field start to the next
    packet start (5.1.5)."""
    c = code(rate, short)
    pk = np.asarray(packets, np.uint8).reshape(-1, 188).copy()
    pk[:, 0] = [0] + [crc8(np.unpackbits(p[1:])) for p in pk[:-1]]
    bits, dfl, upl = np.unpackbits(pk.ravel()), c.kbch - 80, 188 * 8
    return [np.concatenate([bbheader(matype1(TS, ro), dfl, upl, 0x47, (-k * dfl) % upl),
                            bits[k * dfl:(k + 1) * dfl]]) for k in range(len(bits) // dfl)]


@lru_cache(maxsize=None)
def bb_prbs(n):
    """BB scrambling sequence (5.2.2, Figure 5): PRBS 1 + X^14 + X^15, stages 1..15 loaded with
    100101010000000 at every BBFRAME start; each output bit is stage 14 XOR stage 15, fed back
    into stage 1. First outputs 00000011..., as drawn in Figure 5."""
    st, out = 0b000000010101001, np.empty(n, np.uint8)       # stage k held in bit k-1
    for i in range(n):
        b = (st >> 13 ^ st >> 14) & 1
        out[i], st = b, (st << 1 | b) & 0x7FFF
    out.flags.writeable = False
    return out


def bb_scramble(bbf):
    """BB scrambling (5.2.2): XOR the whole Kbch-bit BBFRAME with the PRBS, from its MSB."""
    return bbf ^ bb_prbs(len(bbf))


# ---------------------------------------------------------- FEC (5.3)

def clmul(a, b):
    """Product of GF(2) polynomials held as ints (bit k = coefficient of x^k)."""
    r = 0
    while b:
        if b & 1:
            r ^= a
        a, b = a << 1, b >> 1
    return r


@lru_cache(maxsize=None)
def bch_generator(short, t):
    """g(x) = g1(x) g2(x) ... gt(x) (5.3.1, Table 6a normal, 6b short) as an int. The clause
    says "table 5b" for short frames; Table 6b is meant (5b holds no polynomials)."""
    g = 1
    for ex in BCH_POLYS["short" if short else "normal"][:t]:
        g = clmul(g, sum(1 << e for e in ex))
    return g


def bch_encode(m, rate, short=False):
    """Systematic BCH (5.3.1): c(x) = x^r m(x) + d(x), d(x) = x^r m(x) mod g(x), r = Nbch - Kbch.
    m[0] is m_{Kbch-1}, the first bit sent; parity follows as d_{r-1} .. d_0. Bit-serial LFSR."""
    c = code(rate, short)
    g, r = bch_generator(short, c.t), c.nbch - c.kbch
    assert g.bit_length() - 1 == r
    mask, reg = (1 << r) - 1, 0
    for b in np.asarray(m).tolist():
        fb = (reg >> (r - 1)) ^ b
        reg = ((reg << 1) & mask) ^ (g & mask if fb else 0)
    return np.concatenate([m, int_bits(reg, r)])


@lru_cache(maxsize=None)
def bch_byte_table(short, t):
    """256-entry table for byte-at-a-time BCH division (CRC style): T[v] = v(x) x^r mod g(x)."""
    g = bch_generator(short, t)
    r = g.bit_length() - 1
    table = []
    for v in range(256):
        reg = v << (r - 8)
        for _ in range(8):
            reg = (reg << 1) ^ (g if reg >> (r - 1) & 1 else 0)
        table.append(reg)
    return tuple(table)


def bch_encode_bytes(m, rate, short=False):
    """bch_encode() one byte per step, as firmware would (Kbch is a multiple of 8 in all modes)."""
    c = code(rate, short)
    r = c.nbch - c.kbch
    table, mask, reg = bch_byte_table(short, c.t), (1 << r) - 1, 0
    for byte in np.packbits(m).tolist():
        reg = ((reg << 8) & mask) ^ table[(reg >> (r - 8)) ^ byte]
    return np.concatenate([m, int_bits(reg, r)])


def ldpc_encode(i, rate, short=False):
    """LDPC (5.3.2.1), bit-serial as the standard states it. i = BCH codeword (kldpc bits).
    Information bit i_m accumulates into parity addresses (x + (m mod 360) q) mod M for every x in
    table row floor(m/360), M = nldpc - kldpc; afterwards p_k ^= p_{k-1}, k = 1 .. M-1.
    Returns (i, p)."""
    c = code(rate, short)
    M = c.n - c.nbch
    p = np.zeros(M, np.uint8)
    for m in np.flatnonzero(i):
        p[(c.rows[m // 360] + (m % 360) * c.q) % M] ^= 1    # addresses in a row are distinct
    return np.concatenate([i, np.cumsum(p, dtype=np.int64).astype(np.uint8) & 1])


def ldpc_encode_groups(i, rate, short=False):
    """The same encoder in 360-bit group form (the firmware form); output equals ldpc_encode.

    With M = 360 q write each table address x = r + q c (r = x mod q, c = floor(x/q) < 360).
    Bit j of a group accumulates into (x + j q) mod M = r + q ((c + j) mod 360). Hold the parity
    bits as a q x 360 matrix P[r, t] = p_{r + q t}: group g (u = i[360g : 360g+360]) adds, for
    each x in row g, u rotated by c into row r, i.e. P[r, t] ^= u[(t - c) mod 360].
    The accumulation p_k ^= p_{k-1} in natural order k = r + q t runs down column t and carries
    into column t+1. Word-parallel: prefix-XOR the rows (P[r] ^= P[r-1], all 360 columns at
    once), then XOR into each column t >= 1 the parity of columns 0..t-1, which is the running
    XOR along the last row. Natural output order is column-major (a q x 360 bit transpose)."""
    c = code(rate, short)
    P = np.zeros((c.q, 360), np.uint8)
    for g, xs in enumerate(c.rows):
        u = i[360 * g:360 * (g + 1)]
        for x in xs.tolist():
            P[x % c.q] ^= np.roll(u, x // c.q)        # np.roll(u, c)[t] = u[(t - c) mod 360]
    P = np.bitwise_xor.accumulate(P, axis=0)          # down each column
    carry = np.bitwise_xor.accumulate(P[-1])          # carry[t] = parity of columns 0..t
    P[:, 1:] ^= carry[:-1]
    return np.concatenate([i, P.T.ravel()])


def fecframe(bbf, rate, short=False):
    """BBFRAME (unscrambled, Kbch bits) -> FECFRAME (nldpc bits): BB scrambling, BCH, LDPC
    (Figure 6)."""
    return ldpc_encode_groups(bch_encode(bb_scramble(bbf), rate, short), rate, short)


# ---------------------------------------------------------- mapping and PL framing (5.4, 5.5)

def bit_interleave(fec, rate, modulation="8psk"):
    """5.3.3, Table 8, Figures 7/8: write 3/4/5 columns for 8PSK/16APSK/32APSK, read rows.
    For rate 3/5 read columns 3, 2, 1; for all other rates read 1, 2, 3. Thus the first FEC bit
    is the third symbol bit for rate 3/5 and the first otherwise. QPSK has no interleaver."""
    modcod(rate, modulation)
    fec = np.asarray(fec)
    if modulation == "qpsk":
        return fec.copy()
    assert len(fec) in (16200, 64800)
    columns = fec.reshape(BITS_PER_SYMBOL[modulation], -1)
    return (columns[::-1] if modulation == "8psk" and rate == "3/5" else columns).T.ravel()


def qpsk_map(fec):
    """5.4.1, Figure 9: bits 2k and 2k+1 form symbol k; the first sets the I sign, the second
    the Q sign, 0 -> positive: 00 (+,+), 01 (+,-), 10 (-,+), 11 (-,-), times 1/sqrt2. The bits
    are used unchanged."""
    return fec[0::2].copy(), fec[1::2].copy()


def psk8_map(interleaved):
    """5.4.2, Figure 10: interleaved triples, MSB first, -> unit-energy phase in pi/4 units.
    At phases 0..7 the labels are 001, 000, 100, 110, 010, 011, 111, 101."""
    b = np.asarray(interleaved, np.uint8).reshape(-1, 3)
    return np.array([1, 0, 4, 5, 2, 7, 3, 6], np.uint8)[4 * b[:, 0] + 2 * b[:, 1] + b[:, 2]]


def qpsk_phases(bI, bQ):
    """QPSK sign bits -> phase in pi/4 units (Figure 9): 00=1, 01=7, 10=3, 11=5."""
    return np.array([1, 7, 3, 5], np.uint8)[2 * np.asarray(bI) + np.asarray(bQ)]


@lru_cache(maxsize=None)
def apsk_constellation(rate, modulation):
    """5.4.3/4, Figures 11/12 and Tables 9/10: binary label -> complex point, E=1.
    Each ring's labels are read counter-clockwise from its first angle. Independent
    implementation from the standard; no external transmitter/receiver code is used.
    R1=sqrt(4/(1+3 gamma^2)) or sqrt(8/(1+3 gamma1^2+4 gamma2^2))."""
    modcod(rate, modulation)
    if modulation == "16apsk":
        gamma = APSK16_GAMMA[APSK16_RATES.index(rate)]
        r1 = np.sqrt(4 / (1 + 3 * gamma**2))
        rings = ((r1, np.pi / 4, (12, 14, 15, 13)),
                 (r1 * gamma, np.pi / 12, (4, 0, 8, 10, 2, 6, 7, 3, 11, 9, 1, 5)))
    elif modulation == "32apsk":
        g1, g2 = APSK32_GAMMA[APSK32_RATES.index(rate)]
        r1 = np.sqrt(8 / (1 + 3 * g1**2 + 4 * g2**2))
        rings = ((r1, np.pi / 4, (17, 21, 23, 19)),
                 (r1 * g1, np.pi / 12, (16, 0, 1, 5, 4, 20, 22, 6, 7, 3, 2, 18)),
                 (r1 * g2, 0, (24, 8, 25, 9, 13, 29, 12, 28, 30, 14, 31, 15, 11, 27, 10, 26)))
    else:
        raise ValueError("APSK constellation requires 16apsk or 32apsk")
    out = np.empty(1 << BITS_PER_SYMBOL[modulation], np.complex128)
    for radius, start, labels in rings:
        out[list(labels)] = radius * np.exp(1j * (start + 2 * np.pi * np.arange(len(labels)) /
                                                len(labels)))
    out.flags.writeable = False
    return out


def apsk_map(interleaved, rate, modulation):
    """5.4.3/4: group interleaved bits MSB first, then select the labelled APSK point."""
    constellation = apsk_constellation(rate, modulation)
    width = BITS_PER_SYMBOL[modulation]
    bits = np.asarray(interleaved, np.uint8).reshape(-1, width)
    labels = bits @ (1 << np.arange(width - 1, -1, -1))
    return constellation[labels]


SOF = 0x18D2E82                                   # 5.5.2.1, 26 bits
PLS_SCRAMBLE = 0x719D83C953422DFA                 # 5.5.2.4, 64 bits
PLS_G = (0x55555555, 0x33333333, 0x0F0F0F0F,      # Figure 13b rows, y1 = MSB
         0x00FF00FF, 0x0000FFFF, 0xFFFFFFFF)


def pls_code(modcod, short, pilots):
    """PLS code (5.5.2.2-5.5.2.4): b1..b5 = MODCOD MSB first, b6 = TYPE MSB (1 = short) select
    rows of G: y1..y32 = sum b_k G_k. b7 = TYPE LSB (1 = pilots) gives (y1, y1^b7, y2, y2^b7,
    ..., y32, y32^b7). The 64 bits are XORed with the PLS scrambling sequence. A (64,7) code with
    d_min = 32."""
    b = [(modcod >> (4 - k)) & 1 for k in range(5)] + [int(short)]
    y = 0
    for bk, gk in zip(b, PLS_G):
        y ^= gk if bk else 0
    out = np.repeat(int_bits(y, 32), 2)
    out[1::2] ^= int(pilots)
    return out ^ int_bits(PLS_SCRAMBLE, 64)


def plheader(modcod, short, pilots):
    """PLHEADER (5.5.2): y = SOF + PLS code (90 bits) sent as pi/2-BPSK, with 1-based indices
    I_{2i-1} = Q_{2i-1} = (1 - 2 y_{2i-1})/sqrt2,  I_{2i} = -Q_{2i} = -(1 - 2 y_{2i})/sqrt2.
    0-based even k: y=0 -> (+,+), y=1 -> (-,-), so bI = bQ = y. Odd k: y=0 -> (-,+),
    y=1 -> (+,-), so bI = 1 - y, bQ = y. All 90 symbols are QPSK points. Not PL-scrambled."""
    y = np.concatenate([int_bits(SOF, 26), pls_code(modcod, short, pilots)])
    return y ^ (np.arange(90) & 1).astype(np.uint8), y


def insert_pilots(bI, bQ):
    """Pilot blocks (5.5.3): 36 unmodulated symbols I = Q = 1/sqrt2, i.e. (bI, bQ) = (0, 0),
    after every 16 slots of 90 symbols. A block that would follow the last slot (coincide with
    the next SOF) is not sent, leaving int((S-1)/16) blocks (Figure 13)."""
    S, pilot = len(bI) // 90, np.zeros(36, np.uint8)
    oi, oq = [], []
    for s in range(0, S, 16):
        a, b = 90 * s, 90 * min(s + 16, S)
        oi += [bI[a:b]] + ([pilot] if s + 16 < S else [])
        oq += [bQ[a:b]] + ([pilot] if s + 16 < S else [])
    return np.concatenate(oi), np.concatenate(oq)


GOLD_LEN = 2**18 - 1


@lru_cache(maxsize=2)
def gold_z(n):
    """z_n(i) = x((i + n) mod (2^18-1)) XOR y(i), i = 0 .. 2^18-2 (5.5.4). x: 1 + X^7 + X^18
    from x(0) = 1, x(1..17) = 0; y: 1 + Y^5 + Y^7 + Y^10 + Y^18 from all ones."""
    x, y = bytearray(GOLD_LEN), bytearray(b"\x01" * GOLD_LEN)
    x[0] = 1
    for i in range(GOLD_LEN - 18):
        x[i + 18] = x[i + 7] ^ x[i]
        y[i + 18] = y[i + 10] ^ y[i + 7] ^ y[i + 5] ^ y[i]
    return np.roll(np.frombuffer(bytes(x), np.uint8), -n) ^ np.frombuffer(bytes(y), np.uint8)


def pl_scrambling_sequence(length, n=0):
    """R_n(i) = 2 z_n((i + 131072) mod (2^18-1)) + z_n(i), 0..3 (5.5.4); the complex
    scrambling symbol is C_I + j C_Q = exp(j R_n(i) pi/2)."""
    z, i = gold_z(n), np.arange(length)
    return (2 * z[(i + 131072) % GOLD_LEN] + z[i]).astype(np.uint8)


def pl_scramble(bI, bQ, R):
    """Multiply each symbol by exp(j R pi/2) (5.5.4 table). A multiple of 90 degrees maps the
    QPSK points onto themselves; with I = 1 - 2 bI, Q = 1 - 2 bQ:
      R = 0: ( I,  Q) -> (bI, bQ)          R = 1: (-Q,  I) -> (1 - bQ, bI)
      R = 2: (-I, -Q) -> (1 - bI, 1 - bQ)  R = 3: ( Q, -I) -> (bQ, 1 - bI)"""
    swap = (R & 1).astype(bool)
    i2, q2 = np.where(swap, bQ, bI), np.where(swap, bI, bQ)
    return i2 ^ ((R == 1) | (R == 2)), q2 ^ (R >= 2)


def plframe(fec, rate, short=False, pilots=True, n=0):
    """PLFRAME (5.5, Figure 13) as symbol bits (bI, bQ): PLHEADER, then the XFECFRAME with
    optional pilot blocks, PL-scrambled from the first symbol after the header (the sequence
    restarts in every frame)."""
    bI, bQ = qpsk_map(fec)
    if pilots:
        bI, bQ = insert_pilots(bI, bQ)
    bI, bQ = pl_scramble(bI, bQ, pl_scrambling_sequence(len(bI), n))
    hI, hQ = plheader(code(rate, short).modcod, short, pilots)
    return (np.concatenate([hI, bI]).astype(np.uint8),
            np.concatenate([hQ, bQ]).astype(np.uint8))


def dummy_phases():
    """5.5.1, 5.5.4 (pp. 31, 33): 90-symbol MODCOD 0 header + 36 carrier slots.
    No pilot blocks. Reset PL scrambler after the header, including for dummies."""
    head = qpsk_phases(*plheader(0, False, False))
    body = (1 + 2 * pl_scrambling_sequence(36 * 90, 0)) & 7
    return np.concatenate([head, body]).astype(np.uint8)


def plframe_phases(fec, rate, short=False, pilots=True, n=0, modulation="qpsk"):
    """QPSK/8PSK PLFRAME as uint8 phase indices p, symbol = exp(j p pi/4).
    The interleaver applies only to 8PSK (5.3.3); 90-symbol slots, 36 pilots after each 16
    slots except the last, then phase += 2 R_n modulo 8 (5.5). The header is not scrambled."""
    mc = modcod(rate, modulation)
    if modulation not in ("qpsk", "8psk"):
        raise ValueError("phase indices support QPSK/8PSK; use plframe_symbols for APSK")
    assert len(fec) == code(rate, short).n
    if modulation == "8psk":
        phases = psk8_map(bit_interleave(fec, rate))
    else:
        phases = qpsk_phases(*qpsk_map(fec))
    if pilots:
        S = len(phases) // 90
        out = np.ones(len(phases) + 36 * ((S - 1) // 16), np.uint8)  # pi/4 pilot phase
        k = np.arange(len(phases))
        out[k + 36 * (k // 1440)] = phases
        phases = out
    phases = (phases + 2 * pl_scrambling_sequence(len(phases), n)) & 7
    return np.concatenate([qpsk_phases(*plheader(mc, short, pilots)), phases])


def plframe_symbols(fec, rate, short=False, pilots=True, n=0, modulation="qpsk"):
    """PLFRAME as complex symbols. APSK supports normal frames only.
    5.5: 90-symbol slots, 36 unit-energy pilots after each 16 slots except the last,
    then multiply the body by exp(j R_n pi/2). Header/pilot energy is also one."""
    if modulation in ("qpsk", "8psk"):
        return symbols_from_phases(plframe_phases(fec, rate, short, pilots, n, modulation))
    mc = modcod(rate, modulation)
    if short:
        raise ValueError("APSK reference supports normal frames only")
    assert len(fec) == 64800
    body = apsk_map(bit_interleave(fec, rate, modulation), rate, modulation)
    if pilots:
        slots = len(body) // 90
        out = np.full(len(body) + 36 * ((slots - 1) // 16), (1 + 1j) / np.sqrt(2))
        k = np.arange(len(body))
        out[k + 36 * (k // 1440)] = body
        body = out
    body *= np.array([1, 1j, -1, -1j])[pl_scrambling_sequence(len(body), n)]
    return np.concatenate([symbols(*plheader(mc, short, pilots)), body])


def plframe_length(short, pilots, modulation="qpsk"):
    """Figure 13, Table 11: 90 (S+1) + 36 floor((S-1)/16) symbols with pilots.
    S = nldpc/(90 etaMOD), etaMOD = 2/3/4/5 for QPSK/8PSK/16APSK/32APSK."""
    if modulation not in BITS_PER_SYMBOL:
        raise ValueError(f"unsupported modulation: {modulation}")
    if short and modulation in ("16apsk", "32apsk"):
        raise ValueError("APSK reference supports normal frames only")
    S = (16200 if short else 64800) // (90 * BITS_PER_SYMBOL[modulation])
    return 90 * (S + 1) + (36 * ((S - 1) // 16) if pilots else 0)


def transmit(data, rate, short=False, pilots=True, n=0, ro=0.35):
    """User bits -> concatenated GCS PLFRAMEs (bI, bQ). The bits are cut into DATA FIELDs of
    Kbch - 80 bits; the last field is shorter and padded (5.2.1)."""
    dfl = code(rate, short).kbch - 80
    frames = []
    for k in range(0, max(len(data), 1), dfl):
        fec = fecframe(bbframe(data[k:k + dfl], rate, short, ro), rate, short)
        frames.append(plframe(fec, rate, short, pilots, n))
    return np.concatenate([f[0] for f in frames]), np.concatenate([f[1] for f in frames])


def transmit_phases(data, rate, short=False, pilots=True, n=0, ro=0.35, modulation="qpsk"):
    """User bits -> GCS PLFRAMEs in phase-index form. Final DATA FIELD is zero-padded (5.2.1)."""
    modcod(rate, modulation)
    if modulation not in ("qpsk", "8psk"):
        raise ValueError("phase indices support QPSK/8PSK; use transmit_symbols for APSK")
    dfl = code(rate, short).kbch - 80
    frames = []
    for k in range(0, max(len(data), 1), dfl):
        fec = fecframe(bbframe(data[k:k + dfl], rate, short, ro), rate, short)
        frames.append(plframe_phases(fec, rate, short, pilots, n, modulation))
    return np.concatenate(frames)


def transmit_symbols(data, rate, short=False, pilots=True, n=0, ro=0.35, modulation="qpsk"):
    """User bits -> concatenated GCS PLFRAMEs as complex symbols, including APSK."""
    modcod(rate, modulation)
    plframe_length(short, pilots, modulation)
    dfl = code(rate, short).kbch - 80
    frames = []
    for k in range(0, max(len(data), 1), dfl):
        fec = fecframe(bbframe(data[k:k + dfl], rate, short, ro), rate, short)
        frames.append(plframe_symbols(fec, rate, short, pilots, n, modulation))
    return np.concatenate(frames)


# ---------------------------------------------------------- output

def symbols(bI, bQ):
    """Complex symbols (I + jQ), unit energy."""
    return ((1.0 - 2.0 * bI) + 1j * (1.0 - 2.0 * bQ)) / np.sqrt(2)


def symbols_from_phases(phases):
    """Phase indices p -> unit-energy complex symbols exp(j p pi/4)."""
    return np.exp(1j * np.pi / 4 * np.asarray(phases))
