"""Figure style: MATLAB's dark theme, plain titles, captions in the markdown.

A gray figure around near-black axes, boxed axes with inward ticks on all four sides, a faint
white grid and Helvetica (Arial, Liberation Sans or DejaVu Sans where it is missing). Colours are
bright enough to read on the dark axes. Figures carry short titles only: what the figure shows,
its source and its caveats go in the text that embeds it.
"""
import os
from pathlib import Path
import tempfile

os.environ.setdefault('MPLCONFIGDIR', str(Path(tempfile.gettempdir()) / 'esp-dvbs2-mpl'))

import matplotlib

matplotlib.use('Agg')
import matplotlib.pyplot as plt  # noqa: E402

FIGURE, AXES = '#1f1f1f', '#141414'
INK, MID, DARK = '#d9d9d9', '#8c8c8c', '#5a5a5a'
BLUE, ORANGE, YELLOW, PURPLE = '#2fa8ff', '#ff8c3a', '#ffd23f', '#b48cff'
GREEN, CYAN, RED = '#6fdc5c', '#4fe3e3', '#ff4f5e'
WIDTH = 7.2

plt.style.use('default')
plt.rcParams.update({
    'font.family': 'sans-serif',
    'font.sans-serif': ['Helvetica', 'Arial', 'Liberation Sans', 'DejaVu Sans'],
    'mathtext.fontset': 'dejavusans',
    'font.size': 9,
    'text.color': INK,
    'axes.titlesize': 10,
    'axes.titleweight': 'normal',
    'axes.titlepad': 6,
    'axes.titlecolor': INK,
    'axes.labelsize': 9,
    'axes.labelcolor': INK,
    'axes.labelpad': 4,
    'axes.edgecolor': MID,
    'axes.linewidth': 0.6,
    'axes.facecolor': AXES,
    'axes.grid': True,
    'axes.axisbelow': True,
    'axes.prop_cycle': matplotlib.cycler(color=[BLUE, ORANGE, YELLOW, PURPLE, GREEN, CYAN, RED]),
    'grid.color': '#ffffff',
    'grid.alpha': 0.15,
    'grid.linewidth': 0.5,
    'xtick.color': INK,
    'ytick.color': INK,
    'xtick.labelsize': 8,
    'ytick.labelsize': 8,
    'xtick.direction': 'in',
    'ytick.direction': 'in',
    'xtick.top': True,
    'ytick.right': True,
    'xtick.major.size': 4,
    'ytick.major.size': 4,
    'xtick.major.width': 0.6,
    'ytick.major.width': 0.6,
    'figure.facecolor': FIGURE,
    'savefig.facecolor': FIGURE,
    'figure.dpi': 120,
    'savefig.dpi': 220,
    'savefig.bbox': 'tight',
    'savefig.pad_inches': 0.12,
    'legend.frameon': True,
    'legend.facecolor': AXES,
    'legend.edgecolor': DARK,
    'legend.framealpha': 1,
    'legend.fontsize': 8,
    'lines.linewidth': 1.0,
    'lines.markersize': 4.5,
    'image.cmap': 'magma',
})


def lollipop(ax, y, value, color=BLUE, *, marker='o', label=None, fmt='{:.1f}', offset=5):
    ax.hlines(y, 0, value, color=color, linewidth=1.2)
    ax.plot(value, y, marker=marker, color=color, linestyle='none', label=label)
    ax.annotate(fmt.format(value), (value, y), xytext=(offset, 0),
                textcoords='offset points', va='center', fontsize=8, color=INK,
                ha='left' if offset > 0 else 'right')


def save_figure(fig, path, caption=None):
    """Write a PNG with fixed metadata (no timestamps). The caption belongs in the embedding
    text and is not drawn; callers keep it as a record of what the figure needs said."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(path, metadata={'Software': 'esp-dvbs2 / Matplotlib'})
    plt.close(fig)
    print(path)
