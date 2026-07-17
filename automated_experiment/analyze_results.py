#!/usr/bin/env python3
"""Analyze WebRTC video-streaming experiment results.

Walks the results tree produced by ``automated_experiment.sh``:

    results/<experiment-id>/<traffic-config>/<trace>/webrtc_logs/.../receiver/

and summarizes the receiver-side video-quality metrics, producing comparison
plots across traffic configurations and network traces.

Receiver inputs:
  * average_stats.csv  - periodic aggregate stats (frame rate, bitrate, drops, ...)
  * frame_metrics.csv  - per-frame timing (transport latency, jitter, ...)
"""

import argparse
from collections import defaultdict
from pathlib import Path

import numpy as np
import pandas as pd
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402


def find_receiver_dir(trace_dir: Path):
    """Return the receiver log directory under a trace's webrtc_logs, if any."""
    logs = trace_dir / "webrtc_logs"
    if not logs.exists():
        return None
    for ts_dir in sorted(logs.iterdir()):
        if not ts_dir.is_dir():
            continue
        for session_dir in sorted(ts_dir.iterdir()):
            rx = session_dir / "receiver"
            if rx.is_dir():
                return rx
    return None


def load_results(results_dir: Path):
    """Load receiver metrics as data[config][trace] = {'avg':df, 'frames':df}."""
    data = defaultdict(dict)
    for cfg_dir in sorted(results_dir.iterdir()):
        if not cfg_dir.is_dir() or cfg_dir.name == "profiles":
            continue
        for trace_dir in sorted(cfg_dir.iterdir()):
            if not trace_dir.is_dir() or trace_dir.name == "profiles":
                continue
            rx = find_receiver_dir(trace_dir)
            if rx is None:
                print(f"  [skip] no receiver logs for {cfg_dir.name}/{trace_dir.name}")
                continue
            entry = {}
            for key, name in (("avg", "average_stats.csv"), ("frames", "frame_metrics.csv")):
                path = rx / name
                if path.exists():
                    try:
                        entry[key] = pd.read_csv(path)
                    except Exception as exc:  # noqa: BLE001
                        print(f"  [warn] failed to read {path}: {exc}")
            if entry:
                data[cfg_dir.name][trace_dir.name] = entry
    return data


def _mean(df, col):
    return float(df[col].mean()) if df is not None and col in df.columns and len(df) else float("nan")


def summarize(data):
    """Return a tidy DataFrame of per-(config, trace) video metrics."""
    rows = []
    for config in sorted(data):
        for trace in sorted(data[config]):
            avg = data[config][trace].get("avg")
            frames = data[config][trace].get("frames")

            bitrate_mbps = _mean(avg, "bitrates") / 1e6 if avg is not None and "bitrates" in avg else float("nan")
            row = {
                "config": config,
                "trace": trace,
                "framerate_fps": _mean(avg, "framerate"),
                "bitrate_mbps": bitrate_mbps,
                "frames_dropped": _mean(avg, "frames_dropped"),
                "loss_ratio": _mean(avg, "loss_ratio"),
                "network_ms_mean": _mean(frames, "network_ms"),
                "network_ms_p95": (
                    float(np.nanpercentile(frames["network_ms"], 95))
                    if frames is not None and "network_ms" in frames and len(frames)
                    else float("nan")
                ),
                "frame_jitter_ms_mean": _mean(frames, "frame_jitter_ms"),
            }
            rows.append(row)
    return pd.DataFrame(rows)


def _grouped_bar(summary, metric, ylabel, title, out_path):
    if summary.empty or metric not in summary or summary[metric].isna().all():
        return
    pivot = summary.pivot(index="trace", columns="config", values=metric)
    ax = pivot.plot(kind="bar", figsize=(max(6, 1.2 * len(pivot)), 4))
    ax.set_ylabel(ylabel)
    ax.set_xlabel("network trace")
    ax.set_title(title)
    ax.legend(title="config", fontsize="small")
    plt.tight_layout()
    plt.savefig(out_path, dpi=150)
    plt.close()
    print(f"  wrote {out_path}")


def _latency_cdf(data, column, xlabel, title, out_path):
    plt.figure(figsize=(6, 4))
    plotted = False
    for config in sorted(data):
        pooled = []
        for trace in data[config]:
            frames = data[config][trace].get("frames")
            if frames is not None and column in frames.columns:
                pooled.append(frames[column].dropna().to_numpy())
        if not pooled:
            continue
        values = np.concatenate(pooled)
        if values.size == 0:
            continue
        values = np.sort(values)
        cdf = np.arange(1, values.size + 1) / values.size
        plt.plot(values, cdf, label=config)
        plotted = True
    if not plotted:
        plt.close()
        return
    plt.xlabel(xlabel)
    plt.ylabel("CDF")
    plt.title(title)
    plt.grid(True, alpha=0.3)
    plt.legend(fontsize="small")
    plt.tight_layout()
    plt.savefig(out_path, dpi=150)
    plt.close()
    print(f"  wrote {out_path}")


def main():
    parser = argparse.ArgumentParser(description="Analyze WebRTC video experiment results")
    parser.add_argument("--results-dir", required=True,
                        help="Experiment results directory (e.g. results/my_batch)")
    parser.add_argument("--output-dir", default="plots",
                        help="Directory to write plots and summary.csv (default: plots)")
    args = parser.parse_args()

    results_dir = Path(args.results_dir)
    if not results_dir.is_dir():
        parser.error(f"results directory not found: {results_dir}")
    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    print(f"Loading results from {results_dir} ...")
    data = load_results(results_dir)
    if not data:
        print("No receiver metrics found. Did the experiment produce webrtc_logs?")
        return

    summary = summarize(data)
    summary_path = out_dir / "summary.csv"
    summary.to_csv(summary_path, index=False)
    print(f"\n=== Summary ({summary_path}) ===")
    with pd.option_context("display.max_rows", None, "display.width", 160):
        print(summary.to_string(index=False))

    print("\nGenerating plots ...")
    _grouped_bar(summary, "framerate_fps", "frame rate (fps)",
                 "Receiver frame rate", out_dir / "framerate_comparison.png")
    _grouped_bar(summary, "bitrate_mbps", "bitrate (Mbps)",
                 "Receiver bitrate", out_dir / "bitrate_comparison.png")
    _grouped_bar(summary, "frames_dropped", "frames dropped (avg/interval)",
                 "Dropped frames", out_dir / "frames_dropped_comparison.png")
    _latency_cdf(data, "network_ms", "transport latency (ms)",
                 "Per-frame transport latency", out_dir / "latency_cdf.png")
    _latency_cdf(data, "frame_jitter_ms", "frame jitter (ms)",
                 "Per-frame jitter", out_dir / "frame_jitter_cdf.png")
    print("\nDone.")


if __name__ == "__main__":
    main()
