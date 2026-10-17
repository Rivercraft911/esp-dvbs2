#!/usr/bin/env python3
"""Draw the README figures from the recorded runs in results/. Run: python3 host/plots.py"""
from __future__ import annotations

import json
import pathlib

from style import DARK, INK, MID, RED, WIDTH, plt, save_figure

ROOT = pathlib.Path(__file__).resolve().parent.parent
IMG = ROOT / "docs" / "img"

# DAC bank geometry at 40 MS/s (docs/prior-art.md, esp-sdr-zephyr): a 16380-sample bank loops
# every 409.5 us, and with 64 lead and 2048 tail samples the CPU must hand over a freshly
# filled bank every (16380 - 64 - 2048) / 40e6 = 356.7 us.
SWITCH_US = (16380 - 64 - 2048) / 40.0
DAC_READ_MB_S = 4 * 40.0


def latest(path: pathlib.Path) -> dict[str, dict]:
    """The last record of each membench test in a JSON-lines file."""
    tests = {}
    for line in path.read_text().splitlines():
        record = json.loads(line)
        if record.get("cmd") == "membench" and "test" in record:
            tests[record["test"]] = record
    return tests


def fig_membench():
    t = latest(ROOT / "results" / "2026-10-16-memory" / "runs.jsonl")
    fig, (a, b) = plt.subplots(1, 2, figsize=(WIDTH, 2.3), gridspec_kw=dict(width_ratios=[1.25, 1]))

    fills = [("plain stores", t["bank_store"]), ("table rows", t["bank_rows"]),
             ("I and Q rows, OR (C)", t["bank_rows_or"])]
    for y, (label, r) in enumerate(reversed(fills)):
        us = r["us_per_bank"]
        late = us > SWITCH_US
        a.barh(y, us, 0.55, color="white" if late else "#d9d9d9", edgecolor=INK, lw=0.6,
               hatch="////" if late else None)
        a.text(max(us, SWITCH_US) + 10, y, f"{us:.0f} µs, {r['cycles_per_word']:.2f} cycles/sample",
               va="center", fontsize=7.5, color=INK)
    a.axvline(SWITCH_US, color=RED, ls="--", lw=0.8)
    a.text(SWITCH_US - 6, 2.42, f"refill deadline {SWITCH_US:.0f} µs", ha="right", fontsize=7.5,
           color=RED)
    a.set_yticks(range(3), [f[0] for f in reversed(fills)])
    a.set_xlim(0, 780)
    a.tick_params(axis="y", right=False)
    a.set_ylim(-0.6, 2.75)
    a.set_xlabel("time to fill one 16380-sample DAC bank, one core (µs)")
    a.set_title("(a) Filling the next DAC bank at 40 MS/s", loc="left")

    rates = [("SRAM copy", t["sram_memcpy"]), ("PSRAM read", t["psram_read"]),
             ("PSRAM write", t["psram_write"])]
    for y, (label, r) in enumerate(reversed(rates)):
        mb = r["mb_per_s"]
        b.barh(y, mb, 0.55, color="#d9d9d9" if mb >= DAC_READ_MB_S else "white", edgecolor=INK,
               lw=0.6)
        b.text(mb + 6, y, f"{mb:.0f} MB/s", va="center", fontsize=7.5, color=INK)
    b.axvline(DAC_READ_MB_S, color=MID, ls=":", lw=0.8)
    b.text(DAC_READ_MB_S + 6, 2.42, "DAC reads 160 MB/s", fontsize=7.5, color=DARK)
    b.set_yticks(range(3), [r[0] for r in reversed(rates)])
    b.set_xlim(0, 540)
    b.tick_params(axis="y", right=False)
    b.set_ylim(-0.6, 2.75)
    b.set_xlabel("measured bandwidth (MB/s)")
    b.set_title("(b) Where samples can live", loc="left")
    save_figure(fig, IMG / "membench")


def fig_fecbench():
    """Median cycles per frame by stage for the plain C encoder, against the 5 Msym/s budget."""
    rows = {}
    for line in (ROOT / "results" / "2026-10-16-encoder" / "runs.jsonl").read_text().splitlines():
        r = json.loads(line)
        if r.get("event") == "frame" and r["input"] == "gcs":
            rows.setdefault(f"{r['modulation'].upper()} {r['rate']}", []).append(r["cycles"])
    picks = ["QPSK 2/3", "8PSK 2/3"]
    stages = [(("bb", "bb_scramble"), "BB framing", "white", None), (("bch",), "BCH", "#d9d9d9", None),
              (("ldpc",), "LDPC", "#9a9a9a", None), (("interleave_map",), "interleave + map", "white", "////"),
              (("pl",), "PL framing", "white", "...")]
    budget = 240e6 * 22194 / 5e6 / 1e3          # one core, 8PSK frame at 5 Msym/s, thousands
    fig, ax = plt.subplots(figsize=(WIDTH, 2.1))
    for y, name in enumerate(reversed(picks)):
        frames = rows[name]
        left = 0.0
        for keys, label, color, hatch in stages:
            k = sorted(sum(f[key] for key in keys) for f in frames)[len(frames) // 2] / 1e3
            ax.barh(y, k, 0.5, left=left, color=color, hatch=hatch, edgecolor=INK, lw=0.5,
                    label=label if y == 0 else None)
            left += k
        ax.text(left + 25, y, f"{left / 1e3:.2f} M", va="center", fontsize=7.5, color=INK)
    ax.axvline(budget, color=RED, ls="--", lw=0.8)
    ax.text(budget + 25, -0.42, "one core's budget per frame at 5 Msym/s", fontsize=7.5, color=RED)
    ax.set_yticks(range(len(picks)), list(reversed(picks)))
    ax.tick_params(axis="y", right=False)
    ax.set_xlim(0, 2900)
    ax.set_ylim(-0.55, 1.5)
    ax.set_xlabel("cycles per normal frame, median of 5, thousands")
    ax.legend(loc="lower center", bbox_to_anchor=(0.5, 1.0), ncol=5, fontsize=7, handlelength=1.6)
    save_figure(fig, IMG / "fecbench")


def fig_encoder():
    """8PSK 2/3 cycles per frame by stage: plain C, portable fast C and PIE, against symbol rates."""
    runs = {}
    for line in (ROOT / "results" / "2026-10-16-fec-limit" / "runs.jsonl").read_text().splitlines():
        r = json.loads(line)
        if r.get("event") == "frame" and r["modcod"] == 13 and r["input"] == "gcs":
            runs.setdefault(r["impl"], []).append(r["cycles"])
    picks = [("plain", "plain C"), ("c", "fast, portable C"), ("pie", "fast, PIE")]
    stages = [(("bb", "bb_scramble"), "BB framing", "white", None), (("bch",), "BCH", "#d9d9d9", None),
              (("ldpc",), "LDPC", "#9a9a9a", None), (("interleave_map",), "interleave + map", "white", "////"),
              (("pl",), "PL framing", "white", "...")]
    median = lambda values: sorted(values)[len(values) // 2]
    # One core's cycles per 22,194-symbol 8PSK frame at each symbol rate, thousands.
    budgets = {rate: 240e6 * 22194 / (rate * 1e6) / 1e3 for rate in (5, 10, 20)}
    fig, ax = plt.subplots(figsize=(WIDTH, 2.3))
    for y, (impl, _) in enumerate(reversed(picks)):
        frames = runs[impl]
        left = 0.0
        for keys, label, color, hatch in stages:
            k = median([sum(f[key] for key in keys) for f in frames]) / 1e3
            ax.barh(y, k, 0.5, left=left, color=color, hatch=hatch, edgecolor=INK, lw=0.5,
                    label=label if y == 0 else None)
            left += k
        total = median([f["total"] for f in frames]) / 1e3
        x = left + 25
        for line in budgets.values():
            if x - 10 < line < x + 90:
                x = line + 15
        ax.text(x, y, f"{total:.0f}k", va="center", fontsize=7.5, color=INK)
    for rate, x in budgets.items():
        ax.axvline(x, color=RED, ls="--", lw=0.8)
        ax.text(x + 18, 2.42, f"{rate} Msym/s", fontsize=7.5, color=RED)
    ax.set_yticks(range(len(picks)), [p[1] for p in reversed(picks)])
    ax.tick_params(axis="y", right=False)
    ax.set_xlim(0, 2800)
    ax.set_ylim(-0.55, 2.7)
    ax.set_xlabel("cycles per 8PSK 2/3 normal frame, median of 5, thousands (one core's budget dashed)")
    ax.legend(loc="lower center", bbox_to_anchor=(0.5, 1.0), ncol=5, fontsize=7, handlelength=1.6)
    save_figure(fig, IMG / "encoder")


def fig_shaper():
    """Real DVB-S2 pulse shaping into one DAC bank: plain C against the PIE vector kernels."""
    t = latest(ROOT / "results" / "2026-10-16-shaper" / "runs.jsonl")
    rows = [("8PSK, 8 samples/symbol, C", t["shape8_c"]), ("8PSK, 8 samples/symbol, PIE", t["shape8_pie"]),
            ("QPSK, 40 samples/symbol, C", t["shape40_c"]), ("QPSK, 40 samples/symbol, PIE", t["shape40_pie"])]
    fig, ax = plt.subplots(figsize=(WIDTH, 2.2))
    for y, (label, r) in enumerate(reversed(rows)):
        us = r["us_per_bank"]
        pie = "PIE" in label
        ax.barh(y, us, 0.55, color="#d9d9d9" if pie else "white", edgecolor=INK, lw=0.6,
                hatch=None if pie else "////")
        ax.text(max(us, SWITCH_US) + 12, y, f"{us:.0f} µs, {r['cycles_per_word']:.2f} cycles/sample",
                va="center", fontsize=7.5, color=INK)
    for x, label in ((SWITCH_US, "deadline at 40 MS/s"), (SWITCH_US / 2, "at 80 MS/s")):
        ax.axvline(x, color=RED, ls="--", lw=0.8)
        ax.text(x + 8, 3.48, label, fontsize=7.5, color=RED)
    ax.set_yticks(range(len(rows)), [r[0] for r in reversed(rows)])
    ax.tick_params(axis="y", right=False)
    ax.set_xlim(0, 1450)
    ax.set_ylim(-0.6, 3.8)
    ax.set_xlabel("time to shape one 16380-sample DAC bank on one core (µs)")
    save_figure(fig, IMG / "shaper")


if __name__ == "__main__":
    fig_membench()
    fig_shaper()
    fig_fecbench()
    fig_encoder()
    print("wrote", IMG.relative_to(ROOT))
