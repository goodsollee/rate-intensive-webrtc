#!/usr/bin/env python3
"""Convert .pitree-trace bandwidth traces into emulator CSV profiles."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path


def parse_trace_line(line: str) -> tuple[float, float] | None:
    line = line.strip()
    if not line or line.startswith("#"):
        return None
    parts = line.replace(",", " ").split()
    try:
        time_s = float(parts[0])
    except ValueError as exc:  # pragma: no cover - defensive parsing
        raise ValueError(f"Failed to parse time from line: {line}") from exc

    try:
        mbps = float(parts[1])
    except IndexError as exc:
        raise ValueError(f"Trace line missing throughput value: {line}") from exc
    except ValueError as exc:  # pragma: no cover
        raise ValueError(f"Failed to parse throughput from line: {line}") from exc

    return time_s, mbps


def convert_trace(source: Path, destination: Path, latency_ms: float) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    with source.open("r", encoding="utf-8") as fin, destination.open(
        "w", encoding="utf-8", newline=""
    ) as fout:
        writer = csv.writer(fout)
        writer.writerow(["timestamp_ms", "bandwidth_kbps", "latency_ms"])
        for raw in fin:
            parsed = parse_trace_line(raw)
            if not parsed:
                continue
            time_s, mbps = parsed
            timestamp_ms = int(round(time_s * 1000.0))
            bandwidth_kbps = max(0.0, mbps * 1000.0)
            writer.writerow([timestamp_ms, f"{bandwidth_kbps:.3f}", latency_ms])


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path, help="Path to .pitree-trace file")
    parser.add_argument("output", type=Path, help="Destination CSV profile path")
    parser.add_argument(
        "--latency-ms",
        type=float,
        default=30.0,
        help="Fixed latency in milliseconds to apply to the profile",
    )
    args = parser.parse_args()

    if not args.trace.exists():
        raise SystemExit(f"Trace file not found: {args.trace}")

    convert_trace(args.trace, args.output, args.latency_ms)


if __name__ == "__main__":
    main()
