"""Figure style for the README: serif text, fine boxed axes, grayscale with one accent."""
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

INK, DARK, MID = "#111111", "#444444", "#707070"
BLUE, RED, GREEN = "#0055aa", "#c62828", "#287a3e"
WIDTH = 6.8

plt.style.use("default")
plt.rcParams.update({
    "font.family": "serif",
    "font.serif": ["Times New Roman", "STIXGeneral", "DejaVu Serif"],
    "mathtext.fontset": "stix",
    "font.size": 9,
    "text.color": INK,
    "axes.titlesize": 9.5,
    "axes.titleweight": "normal",
    "axes.titlelocation": "center",
    "axes.titlepad": 7,
    "axes.titlecolor": INK,
    "axes.labelcolor": INK,
    "axes.labelsize": 9,
    "axes.labelpad": 5,
    "axes.facecolor": "white",
    "axes.edgecolor": INK,
    "axes.linewidth": 0.6,
    "axes.spines.top": True,
    "axes.spines.right": True,
    "axes.xmargin": 0.03,
    "axes.ymargin": 0.05,
    "axes.grid": False,
    "axes.prop_cycle": matplotlib.cycler(color=[INK, RED, BLUE, GREEN]),
    "xtick.color": INK,
    "ytick.color": INK,
    "xtick.labelsize": 8,
    "ytick.labelsize": 8,
    "xtick.direction": "in",
    "ytick.direction": "in",
    "xtick.top": True,
    "ytick.right": True,
    "xtick.major.size": 3,
    "ytick.major.size": 3,
    "xtick.minor.size": 1.5,
    "ytick.minor.size": 1.5,
    "xtick.major.width": 0.6,
    "ytick.major.width": 0.6,
    "xtick.minor.width": 0.4,
    "ytick.minor.width": 0.4,
    "xtick.major.pad": 4,
    "ytick.major.pad": 4,
    "grid.color": "#dddddd",
    "grid.linewidth": 0.35,
    "grid.linestyle": ":",
    "figure.facecolor": "white",
    "savefig.facecolor": "white",
    "figure.dpi": 120,
    "savefig.dpi": 250,
    "savefig.bbox": "tight",
    "savefig.pad_inches": 0.08,
    "legend.frameon": False,
    "legend.fontsize": 8,
    "legend.labelcolor": INK,
    "legend.handlelength": 2.8,
    "legend.borderaxespad": 0.7,
    "lines.linewidth": 0.9,
    "lines.markersize": 3.2,
    "lines.markeredgewidth": 0.6,
    "lines.markerfacecolor": "white",
    "lines.solid_capstyle": "butt",
    "lines.dash_capstyle": "butt",
    "pdf.fonttype": 42,
    "ps.fonttype": 42,
    "svg.fonttype": "path",
})


def dots(ax, x, y, color=INK, label=None, *, marker="o", linestyle="-", **kw):
    """Thin series with small open markers; distinguish series without colour."""
    return ax.plot(x, y, color=color, label=label, marker=marker, linestyle=linestyle, **kw)


def lollipop(ax, y, x, color=INK, fmt="{:.0f} %", marker="o"):
    """Fine horizontal stem, open marker and compact numeric label."""
    ax.hlines(y, 0, x, color=color, lw=0.7)
    ax.plot([x], [y], marker=marker, color=color, ms=3.2)
    ax.annotate(fmt.format(x), (x, y), xytext=(5, 0), textcoords="offset points",
                va="center", fontsize=8, color=INK)


def save_figure(fig, path, caption=None):
    """Write one PNG for the README."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    caption_height = 0.38 if caption and "\n" in caption else 0.22 if caption else 0
    bottom = caption_height / fig.get_figheight()
    fig.tight_layout(rect=(0, bottom, 1, 0.98))
    if caption:
        fig.text(0.5, 0.025, caption, ha="center", va="bottom", fontsize=7.5,
                 linespacing=1.3, color=DARK)
    fig.savefig(path.with_suffix(".png"))
    plt.close(fig)
