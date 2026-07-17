"""Renderer implementations used by the optional plotting extension."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Dict, Iterable, List, Optional, Sequence

import matplotlib.pyplot as plt
import numpy as np

from .presets import FIGSIZE
from .spec_types import FigureEntry, FigureSpec


SaveFn = Callable[[plt.Figure, Path, Sequence[str]], None]


@dataclass
class _RenderContext:
    analyzer: "ExperimentAnalyzer"
    figure: FigureEntry
    configs: List[str]
    traces: List[str]
    figsize: Optional[Sequence[float]]
    output_dir: Path
    output_path: Path
    export_formats: Sequence[str]
    save: SaveFn


class PlotRegistry:
    """Registry for named figure builders."""

    _registry: Dict[str, Callable[[_RenderContext], None]] = {}
    _legacy_outputs: Dict[str, str] = {
        "bitrate_comparison": "bitrate_comparison.png",
        "framerate_comparison": "framerate_comparison.png",
        "framerate_cdf_overall": "framerate_cdf_overall.png",
    }

    @classmethod
    def register(cls, kind: str):
        def decorator(func: Callable[[_RenderContext], None]):
            cls._registry[kind] = func
            return func

        return decorator

    @classmethod
    def render_from_spec(
        cls,
        analyzer,
        spec: FigureSpec,
        default_output_dir: str,
        default_figsize: Optional[Sequence[float]],
        include_configs: Optional[Iterable[str]] = None,
        include_traces: Optional[Iterable[str]] = None,
        exclude_traces: Optional[Iterable[str]] = None,
    ) -> None:
        base_output_dir = Path(spec.defaults.output_dir or default_output_dir)
        base_output_dir.mkdir(parents=True, exist_ok=True)

        include_configs = set(include_configs or []) or None
        include_traces = set(include_traces or []) or None
        exclude_traces = set(exclude_traces or []) or None

        for figure in spec.figures:
            renderer = cls._registry.get(figure.kind)
            if renderer is None:
                continue

            configs = cls._select_configs(analyzer, figure, include_configs)
            if not configs:
                continue

            traces = cls._select_traces(analyzer, configs, figure, include_traces, exclude_traces)
            if not traces:
                continue

            figure_output_dir = Path(figure.options.get("output_dir") or base_output_dir)
            figure_output_dir.mkdir(parents=True, exist_ok=True)

            output_name = cls._determine_output_name(figure)
            export_formats = cls._determine_export_formats(figure, output_name)

            figsize = cls._resolve_figsize(figure, spec, default_figsize)

            output_path = Path(output_name)

            context = _RenderContext(
                analyzer=analyzer,
                figure=figure,
                configs=configs,
                traces=traces,
                figsize=figsize,
                output_dir=figure_output_dir,
                output_path=output_path,
                export_formats=export_formats,
                save=lambda fig, path, formats: cls._save_figure(
                    fig,
                    path if path.is_absolute() else figure_output_dir / path,
                    formats,
                ),
            )

            renderer(context)

    @staticmethod
    def _resolve_figsize(
        figure: FigureEntry, spec: FigureSpec, default_figsize: Optional[Sequence[float]]
    ) -> Optional[Sequence[float]]:
        size_key = figure.options.get("size") if figure.options else None
        if size_key is None and spec.defaults.size:
            size_key = spec.defaults.size
        if size_key is None:
            return default_figsize
        return FIGSIZE.get(size_key, default_figsize)

    @classmethod
    def _select_configs(
        cls,
        analyzer,
        figure: FigureEntry,
        include_configs: Optional[set],
    ) -> List[str]:
        configs = sorted([c for c in analyzer.data.keys() if not str(c).startswith("_")])
        if include_configs:
            configs = [c for c in configs if c in include_configs]

        figure_include = set(figure.include.get("configs", [])) if figure.include else set()
        if figure_include:
            configs = [c for c in configs if c in figure_include]

        figure_exclude = set(figure.exclude.get("configs", [])) if figure.exclude else set()
        if figure_exclude:
            configs = [c for c in configs if c not in figure_exclude]
        return configs

    @classmethod
    def _select_traces(
        cls,
        analyzer,
        configs: List[str],
        figure: FigureEntry,
        include_traces: Optional[set],
        exclude_traces: Optional[set],
    ) -> List[str]:
        all_traces: set = set()
        for config in configs:
            all_traces.update(
                trace for trace in analyzer.data.get(config, {}).keys() if not str(trace).startswith("_")
            )
        traces = sorted(all_traces)

        if include_traces:
            traces = [t for t in traces if t in include_traces]

        if figure.include:
            fig_traces = set(figure.include.get("traces", []) or [])
            if fig_traces:
                traces = [t for t in traces if t in fig_traces]

        excluded = set(figure.exclude.get("traces", []) or [])
        if exclude_traces:
            excluded |= exclude_traces
        if excluded:
            traces = [t for t in traces if t not in excluded]

        return traces

    @classmethod
    def _determine_output_name(cls, figure: FigureEntry) -> str:
        if figure.output:
            return figure.output
        return cls._legacy_outputs.get(figure.id, f"{figure.id}.png")

    @staticmethod
    def _determine_export_formats(figure: FigureEntry, output_name: str) -> Sequence[str]:
        if figure.export:
            return list(dict.fromkeys(fmt.lstrip(".") for fmt in figure.export))
        suffix = Path(output_name).suffix.lstrip(".")
        return [suffix or "png"]

    @staticmethod
    def _save_figure(fig: plt.Figure, target: Path, export_formats: Sequence[str]) -> None:
        target = Path(target)
        target.parent.mkdir(parents=True, exist_ok=True)
        for fmt in export_formats:
            fmt = fmt.lstrip(".")
            out_path = target if target.suffix else target.with_suffix(f".{fmt}")
            if target.suffix and target.suffix != f".{fmt}":
                out_path = target.with_suffix(f".{fmt}")
            fig.savefig(out_path, bbox_inches="tight")
        plt.close(fig)


@PlotRegistry.register("groupedbar")
def _render_groupedbar(context: _RenderContext) -> None:
    metric = context.figure.metric
    if not metric:
        return

    configs = context.configs
    traces = context.traces

    metric_data = {config: {"means": [], "stds": []} for config in configs}

    for trace in traces:
        for config in configs:
            if trace in context.analyzer.data.get(config, {}):
                func_name = f"calculate_{metric}"
                if not hasattr(context.analyzer, func_name):
                    mean_val = std_val = 0
                else:
                    mean_val, std_val = getattr(context.analyzer, func_name)(config, trace)
                    mean_val = mean_val if mean_val is not None else 0
                    std_val = std_val if std_val is not None else 0
            else:
                mean_val = std_val = 0
            metric_data[config]["means"].append(mean_val)
            metric_data[config]["stds"].append(std_val)

    figsize = context.figsize or (6.6, 2.3)
    fig, ax = plt.subplots(figsize=figsize)

    x = np.arange(len(traces))
    width = 0.8 / max(len(configs), 1)

    for idx, config in enumerate(configs):
        offset = (idx - len(configs) / 2 + 0.5) * width
        ax.bar(
            x + offset,
            metric_data[config]["means"],
            width,
            yerr=metric_data[config]["stds"],
            label=config,
            capsize=3,
        )

    ax.set_xlabel(context.figure.xlabel or "Trace")
    ax.set_ylabel(context.figure.ylabel or "Value")
    ax.set_title(context.figure.title or context.figure.id.replace("_", " ").title())
    ax.set_xticks(x)
    ax.set_xticklabels(traces, rotation=45, ha="right")
    ax.legend()
    ax.margins(x=0.01)

    context.save(fig, context.output_path, context.export_formats)


@PlotRegistry.register("bar")
def _render_bar(context: _RenderContext) -> None:
    metric = context.figure.metric
    if not metric:
        return

    configs = context.configs
    traces = context.traces

    func_name = f"calculate_{metric}"
    if not hasattr(context.analyzer, func_name):
        return

    values = []
    labels = []

    for config in configs:
        for trace in traces:
            if trace not in context.analyzer.data.get(config, {}):
                continue
            mean_val, _ = getattr(context.analyzer, func_name)(config, trace)
            if mean_val is None:
                continue
            values.append(mean_val)
            labels.append(f"{config}\n{trace}")

    if not values:
        return

    figsize = context.figsize or (3.3, 2.3)
    fig, ax = plt.subplots(figsize=figsize)
    x = np.arange(len(values))
    ax.bar(x, values)
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=45, ha="right")
    ax.set_ylabel(context.figure.ylabel or metric.replace("_", " ").title())
    ax.set_title(context.figure.title or context.figure.id.replace("_", " ").title())

    context.save(fig, context.output_path, context.export_formats)


@PlotRegistry.register("cdf")
def _render_cdf(context: _RenderContext) -> None:
    metric = context.figure.metric
    if not metric:
        return

    func = getattr(context.analyzer, f"get_{metric}", None)
    if func is None and metric == "framerate_cdf":
        func = getattr(context.analyzer, "get_capped_framerate_series", None)
    if func is None:
        return

    figsize = context.figsize or (6.6, 2.3)
    fig, ax = plt.subplots(figsize=figsize)

    for config in context.configs:
        all_values = []
        for trace in context.traces:
            entry = func(config, trace)
            if entry is None:
                continue
            if isinstance(entry, np.ndarray):
                values = entry
            else:
                values = np.asarray(entry)
            if values.size:
                all_values.append(values)
        if not all_values:
            continue
        merged = np.concatenate(all_values)
        if merged.size == 0:
            continue
        x = np.sort(merged)
        y = np.arange(1, x.size + 1) / x.size
        ax.plot(x, y, label=config, linewidth=2)

    ax.set_xlabel(context.figure.xlabel or "Value")
    ax.set_ylabel(context.figure.ylabel or "ECDF")
    ax.set_title(context.figure.title or context.figure.id.replace("_", " ").title())
    ax.set_ylim(0, 1)
    ax.legend(title="Config")

    context.save(fig, context.output_path, context.export_formats)


@PlotRegistry.register("timeline")
def _render_timeline(context: _RenderContext) -> None:
    figsize = context.figsize or (6.6, 2.3)

    for config in context.configs:
        config_dir = context.output_dir / config / "timeline"
        config_dir.mkdir(parents=True, exist_ok=True)
        for trace in context.traces:
            entry = context.analyzer.data.get(config, {}).get(trace)
            if not entry:
                continue

            has_rtp = "average_stats" in entry and "timestamp_ms" in entry["average_stats"].columns
            if not has_rtp:
                continue

            min_time_ms = entry["average_stats"]["timestamp_ms"].min()
            max_time_ms = entry["average_stats"]["timestamp_ms"].max()
            if np.isnan(min_time_ms) or np.isnan(max_time_ms):
                continue
            max_time_s = (max_time_ms - min_time_ms) / 1000.0

            exp_name = f"{config}_{trace}"

            # Overview timeline (bandwidth + totals)
            fig1, ax1 = plt.subplots(figsize=(figsize[0] * 2, figsize[1] * 2))

            if "bandwidth" in entry:
                df_bw = entry["bandwidth"]
                if {"elapsed_ms", "bandwidth_kbps"}.issubset(df_bw.columns):
                    time_s = (df_bw["elapsed_ms"] - min_time_ms) / 1000.0
                    ax1.plot(time_s, df_bw["bandwidth_kbps"] / 1000.0, label="Available Bandwidth", linestyle="--", alpha=0.8)

            df_rtp = entry["average_stats"]
            time_s = (df_rtp["timestamp_ms"] - min_time_ms) / 1000.0
            if "bitrates" in df_rtp.columns:
                ax1.plot(time_s, df_rtp["bitrates"] / 1_000_000, label="RTP Total")

            if "rtp_throughput" in entry:
                df_sctp_total = entry["rtp_throughput"]
                if {"timestamp_ms", "receive_throughput_mbps"}.issubset(df_sctp_total.columns):
                    time_s = (df_sctp_total["timestamp_ms"] - min_time_ms) / 1000.0
                    ax1.plot(time_s, df_sctp_total["receive_throughput_mbps"], label="SCTP Total")

            ax1.set_xlim(0, max_time_s)
            ax1.set_xlabel("Time (s)")
            ax1.set_ylabel("Throughput (Mbps)")
            ax1.set_title(f"Bandwidth and Total Throughput Timeline — {exp_name}")
            ax1.legend(loc="best")

            overview_output = config_dir / f"{exp_name}_overview_timeline.png"
            context.save(fig1, overview_output.relative_to(context.output_dir), context.export_formats)

            # SCTP flow breakdown timeline
            if "sctp_throughput" not in entry:
                continue

            fig2, ax2 = plt.subplots(figsize=(figsize[0] * 2, figsize[1] * 2))
            for flow_name, df_flow in sorted(entry["sctp_throughput"].items()):
                if {"timestamp_ms", "receive_throughput_mbps"}.issubset(df_flow.columns):
                    time_s = (df_flow["timestamp_ms"] - min_time_ms) / 1000.0
                    ax2.plot(time_s, df_flow["receive_throughput_mbps"], label=flow_name)

            ax2.set_xlim(0, max_time_s)
            ax2.set_xlabel("Time (s)")
            ax2.set_ylabel("Throughput (Mbps)")
            ax2.set_title(f"Individual SCTP Flow Throughput Timeline — {exp_name}")
            ax2.legend(loc="best")

            flows_output = config_dir / f"{exp_name}_sctp_flows_timeline.png"
            context.save(fig2, flows_output.relative_to(context.output_dir), context.export_formats)


__all__ = ["PlotRegistry"]
