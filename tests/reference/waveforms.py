"""Independent encoder symbols for the fractional stream convolution check."""
import functools
import numpy as np
import dvbs2
import morse
LOOP_FRAMES = 8
SEED = 2026

def mode(modcod: int) -> tuple[str, str]:
    if 4 <= modcod <= 11:
        return dvbs2.RATES[modcod - 1], "qpsk"
    if 12 <= modcod <= 17:
        return dvbs2.PSK8_RATES[modcod - 12], "8psk"
    raise ValueError("PSK modcods 4..17 only")


def frame_symbols(modcod: int) -> int:
    return dvbs2.plframe_length(False, True, mode(modcod)[1])


def payload(modcod: int, frames: int = LOOP_FRAMES, callsign: str | None = None) -> bytes:
    """GCS payload of data frames, as dvbtx.c fills them (dummies carry no bytes)."""
    if not 4 <= modcod <= 17:
        raise ValueError("PSK modcods 4..17 only")
    if frames < 1:
        raise ValueError("need at least one data frame")
    per_frame = (dvbs2.code(mode(modcod)[0]).kbch - 80) // 8
    out, x = bytearray(frames * per_frame), SEED
    for i in range(len(out)):
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        out[i] = x & 0xFF
    if callsign:
        header = ("ESP-DVBS2 ID=" + morse.callsign(callsign)).ljust(39).encode("ascii") + b"\n"
        for first in range(0, len(out), per_frame):
            out[first:first + 40] = header
    return bytes(out)


@functools.lru_cache(maxsize=8)
def symbols(modcod: int, frames: int = LOOP_FRAMES, dummy_before: tuple[int, ...] = (),
            callsign: str | None = None, rolloff: float = .35) -> np.ndarray:
    """The first `frames` PL frames as complex symbols, from the independent Python encoder."""
    rate, modulation = mode(modcod)
    data = np.frombuffer(payload(modcod, frames, callsign), np.uint8)
    per_frame = len(data) // frames
    if any(f < 0 or f > frames for f in dummy_before):
        raise ValueError("dummy positions must be in 0..frames")
    out = []
    for f in range(frames + 1):
        out.extend([dvbs2.dummy_phases()] * dummy_before.count(f))
        if f == frames:
            break
        bits = np.unpackbits(data[f * per_frame:(f + 1) * per_frame])
        fec = dvbs2.fecframe(dvbs2.bbframe(bits, rate, ro=rolloff), rate)
        out.append(dvbs2.plframe_phases(fec, rate, modulation=modulation))
    return np.exp(1j * np.pi / 4 * np.concatenate(out).astype(float))


def stream_match(stream: bytes, got: bytes, per_frame: int) -> int | None:
    """Offset of `got` in `stream` if it is whole frames starting on a frame boundary, else None."""
    if not got or len(got) % per_frame:
        return None
    at = stream.find(got)
    while at >= 0 and at % per_frame:
        at = stream.find(got, at + 1)
    return at if at >= 0 else None
