"""Dataclasses representing the optional plotting specification format."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional


@dataclass
class FigureDefaults:
    style: Optional[str] = None
    size: Optional[str] = None
    output_dir: Optional[str] = None


@dataclass
class FigureEntry:
    id: str
    kind: str
    title: Optional[str] = None
    metric: Optional[str] = None
    ylabel: Optional[str] = None
    xlabel: Optional[str] = None
    output: Optional[str] = None
    export: Optional[List[str]] = None
    include: Dict[str, List[str]] = field(default_factory=dict)
    exclude: Dict[str, List[str]] = field(default_factory=dict)
    options: Dict[str, Any] = field(default_factory=dict)


@dataclass
class FigureSpec:
    defaults: FigureDefaults = field(default_factory=FigureDefaults)
    figures: List[FigureEntry] = field(default_factory=list)
