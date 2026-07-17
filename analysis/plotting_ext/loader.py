"""Helpers to load optional plotting specifications."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any, Dict, Optional

from .spec_types import FigureDefaults, FigureEntry, FigureSpec


def _load_raw_spec(spec_path: Path) -> Dict[str, Any]:
    text = spec_path.read_text()
    suffix = spec_path.suffix.lower()

    if suffix in {".yaml", ".yml"}:
        try:
            import yaml  # type: ignore
        except Exception as exc:  # pragma: no cover - optional dependency
            raise RuntimeError(
                "PyYAML is required to load YAML specifications"
            ) from exc
        return yaml.safe_load(text) or {}

    if suffix == ".json":
        return json.loads(text or "{}")

    # Try YAML first and then JSON regardless of suffix
    try:
        import yaml  # type: ignore
    except Exception:
        pass
    else:
        loaded = yaml.safe_load(text)
        if loaded is not None:
            return loaded

    return json.loads(text or "{}")


def load_figure_spec(path: Optional[str]) -> Optional[FigureSpec]:
    """Load a figure specification from YAML or JSON.

    Returns ``None`` if ``path`` is falsy or the file does not exist.
    """

    if not path:
        return None

    spec_path = Path(path)
    if not spec_path.exists():
        return None

    raw = _load_raw_spec(spec_path)

    defaults_data = raw.get("defaults", {})
    defaults = FigureDefaults(
        style=defaults_data.get("style"),
        size=defaults_data.get("size"),
        output_dir=defaults_data.get("output_dir"),
    )

    figures = []
    for entry in raw.get("figures", []):
        if not isinstance(entry, dict):
            continue
        try:
            figure = FigureEntry(
                id=str(entry["id"]),
                kind=str(entry.get("kind", "")),
                title=entry.get("title"),
                metric=entry.get("metric"),
                ylabel=entry.get("ylabel"),
                xlabel=entry.get("xlabel"),
                output=entry.get("output"),
                export=entry.get("export"),
                include=entry.get("include", {}) or {},
                exclude=entry.get("exclude", {}) or {},
                options=entry.get("options", {}) or {},
            )
        except KeyError:
            continue
        figures.append(figure)

    return FigureSpec(defaults=defaults, figures=figures)
