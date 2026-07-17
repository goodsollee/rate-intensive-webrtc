#!/usr/bin/env python3
"""Analyze RTP and SCTP log files produced by the benchmark.

The RTP log is expected to contain columns: timestamp, bitrate_bps, fps.
The SCTP log should contain columns: traffic_name, timestamp, size_bytes, latency_ms.
"""

import csv
import math
import statistics
import sys
from typing import List, Tuple


def _load_numeric(reader: csv.DictReader, field: str) -> List[float]:
    values = []
    for row in reader:
        try:
            values.append(float(row.get(field, 0)))
        except ValueError:
            values.append(0.0)
    return values


def analyze_rtp(path: str) -> Tuple[Tuple[float, float], Tuple[float, float]]:
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        bitrates = _load_numeric(reader, "bitrate_bps")
        f.seek(0)
        reader = csv.DictReader(f)
        fps = _load_numeric(reader, "fps")
    return _avg_tail(bitrates), _avg_tail(fps)


def _avg_tail(values: List[float]) -> Tuple[float, float]:
    if not values:
        return 0.0, 0.0
    avg = sum(values) / len(values)
    tail_index = max(int(math.ceil(0.95 * len(values))) - 1, 0)
    tail = sorted(values)[tail_index]
    return avg, tail


def analyze_sctp(path: str):
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        latency = _load_numeric(reader, "latency_ms")
        f.seek(0)
        reader = csv.DictReader(f)
        bandwidth = _load_numeric(reader, "size_bytes")
    sla = sum(1 for l in latency if l <= 100) / len(latency) if latency else 0
    lat_avg, lat_std = _avg_std(latency)
    bw_avg, bw_std = _avg_std(bandwidth)
    return sla, (lat_avg, lat_std), (bw_avg, bw_std)


def _avg_std(values: List[float]) -> Tuple[float, float]:
    if not values:
        return 0.0, 0.0
    return statistics.mean(values), statistics.pstdev(values)


def main(argv):
    if len(argv) != 3:
        print("Usage: analyze_logs.py <rtp_log.csv> <sctp_log.csv>")
        return 1
    rtp_stats = analyze_rtp(argv[1])
    sctp_stats = analyze_sctp(argv[2])
    print("RTP bitrate avg={:.2f}bps tail95={:.2f}bps".format(*rtp_stats[0]))
    print("RTP fps     avg={:.2f}fps tail95={:.2f}fps".format(*rtp_stats[1]))
    sla, lat, bw = sctp_stats
    print("SCTP SLA satisfaction={:.2%}".format(sla))
    print("SCTP latency avg={:.2f}ms std={:.2f}ms".format(lat[0], lat[1]))
    print("SCTP bandwidth avg={:.2f}B std={:.2f}B".format(bw[0], bw[1]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

