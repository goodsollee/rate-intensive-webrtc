"""Matplotlib styling helpers for ACM-ready figures."""

from __future__ import annotations

import contextlib
from typing import Dict, Iterable, Tuple

import matplotlib as mpl


_DEFAULT_ACM_PARAMS: Dict[str, object] = {
    "font.size": 9,
    "axes.titlesize": 10,
    "axes.labelsize": 9,
    "legend.fontsize": 8,
    "xtick.labelsize": 8,
    "ytick.labelsize": 8,
    "grid.alpha": 0.3,
    "axes.grid": True,
    "axes.grid.axis": "y",
    "axes.spines.top": False,
    "axes.spines.right": False,
    "savefig.dpi": 300,
    "figure.dpi": 300,
}

# Color palette inspired by ACM recommended colors (colorblind friendly)
_DEFAULT_ACM_COLORS: Tuple[str, ...] = (
    "#1f77b4",
    "#ff7f0e",
    "#2ca02c",
    "#d62728",
    "#9467bd",
    "#8c564b",
    "#e377c2",
    "#7f7f7f",
    "#bcbd22",
    "#17becf",
)


@contextlib.contextmanager
def apply_acm_style(extra_params: Iterable[Tuple[str, object]] | None = None):
    """Temporarily apply ACM-friendly Matplotlib rcParams."""

    params = dict(_DEFAULT_ACM_PARAMS)
    if extra_params:
        params.update(dict(extra_params))

    old_params = mpl.rcParams.copy()
    try:
        mpl.rcParams.update(params)
        mpl.rcParams["axes.prop_cycle"] = mpl.cycler(color=_DEFAULT_ACM_COLORS)
        yield
    finally:
        mpl.rcParams.update(old_params)
