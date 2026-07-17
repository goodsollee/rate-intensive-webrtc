"""Optional plotting extensions for experiment analysis."""

from .renderers import PlotRegistry  # noqa: F401
from .style_acm import apply_acm_style  # noqa: F401
from .presets import FIGSIZE  # noqa: F401
from .loader import load_figure_spec  # noqa: F401

__all__ = [
    "PlotRegistry",
    "apply_acm_style",
    "FIGSIZE",
    "load_figure_spec",
]
