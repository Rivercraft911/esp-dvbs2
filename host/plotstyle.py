"""Paper-style figures, adapted from RP2350_IQ_Benchmark/reference/plotstyle.py.

Source revision: 88ab018e65a51805ff562842177a4d47b1b04b2e (same author).
See docs/prior-art.md. Serif text, fine boxed axes, inward ticks and open markers. Use Matplotlib's
bundled STIX font so rendering does not depend on installed system fonts.
"""
import os
from pathlib import Path
import tempfile

os.environ.setdefault('MPLCONFIGDIR', str(Path(tempfile.gettempdir()) / 'esp-dvbs2-mpl'))

import matplotlib

matplotlib.use('Agg')
import matplotlib.pyplot as plt  # noqa: E402

INK, DARK, MID = '#111111', '#444444', '#707070'
BLUE, RED, GREEN = '#0055aa', '#c62828', '#287a3e'
WIDTH = 6.8

plt.style.use('default')
plt.rcParams.update({
    'font.family': 'serif',
    'font.serif': ['STIXGeneral'],
    'mathtext.fontset': 'stix',
    'font.size': 9,
    'text.color': INK,
    'axes.titlesize': 9.5,
    'axes.titleweight': 'normal',
    'axes.titlepad': 7,
    'axes.labelsize': 9,
    'axes.labelpad': 5,
    'axes.edgecolor': INK,
    'axes.linewidth': 0.6,
    'axes.spines.top': True,
    'axes.spines.right': True,
    'axes.prop_cycle': matplotlib.cycler(color=[INK, RED, BLUE, GREEN]),
    'xtick.color': INK,
    'ytick.color': INK,
    'xtick.labelsize': 8,
    'ytick.labelsize': 8,
    'xtick.direction': 'in',
    'ytick.direction': 'in',
    'xtick.top': True,
    'ytick.right': True,
    'xtick.major.size': 3,
    'ytick.major.size': 3,
    'xtick.major.width': 0.6,
    'ytick.major.width': 0.6,
    'grid.color': '#dddddd',
    'grid.linewidth': 0.35,
    'grid.linestyle': ':',
    'figure.facecolor': 'white',
    'axes.facecolor': 'white',
    'savefig.facecolor': 'white',
    'figure.dpi': 120,
    'savefig.dpi': 300,
    'savefig.bbox': 'tight',
    'savefig.pad_inches': 0.08,
    'legend.frameon': False,
    'legend.fontsize': 8,
    'lines.linewidth': 0.9,
    'lines.markersize': 4,
    'lines.markeredgewidth': 0.7,
    'lines.markerfacecolor': 'white',
})


def lollipop(ax, y, value, color=BLUE, *, marker='o', label=None, fmt='{:.1f}', offset=5):
    ax.hlines(y, 0, value, color=color, linewidth=0.7)
    ax.plot(value, y, marker=marker, color=color, linestyle='none', label=label)
    ax.annotate(fmt.format(value), (value, y), xytext=(offset, 0),
                textcoords='offset points', va='center', fontsize=8,
                ha='left' if offset > 0 else 'right',
                bbox={'facecolor': 'white', 'edgecolor': 'none', 'pad': 0.2})


def save_figure(fig, path, caption):
    """Fixed layout and PNG metadata; no timestamps or random inputs."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    bottom = (0.38 if '\n' in caption else 0.22) / fig.get_figheight()
    fig.tight_layout(rect=(0, bottom, 1, 0.98))
    fig.text(0.5, 0.025, caption, ha='center', va='bottom', fontsize=7.5,
             linespacing=1.3, color=DARK)
    fig.savefig(path, metadata={'Software': 'esp-dvbs2 / Matplotlib'})
    plt.close(fig)
    print(path)
