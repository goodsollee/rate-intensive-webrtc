import argparse
import csv
import os
from typing import List

import matplotlib.pyplot as plt


def read_column(path: str, column: str) -> List[float]:
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        return [float(row[column]) for row in reader]


def read_csv(path: str) -> List[dict]:
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Plot RTP bitrate and SCTP throughput"
    )
    parser.add_argument(
        "log_dir",
        help="Receiver log directory containing average_stats.csv and sctp_traffic.csv",
    )
    parser.add_argument("--output", default="webrtc_plot.png", help="Output image filename")
    args = parser.parse_args()

    avg_path = os.path.join(args.log_dir, "average_stats.csv")
    sctp_path = os.path.join(args.log_dir, "sctp_traffic.csv")

    if not os.path.exists(avg_path) or not os.path.exists(sctp_path):
        raise FileNotFoundError(f"Log files not found in {args.log_dir}")

    avg_rows = read_csv(avg_path)
    sctp_rows = read_csv(sctp_path)

    avg_times = [
        (float(r["timestamp_ms"]) - float(avg_rows[0]["timestamp_ms"])) / 1000.0
        for r in avg_rows
    ]
    bitrates = [float(r["bitrates"]) / 1e6 for r in avg_rows]

    sctp_times = [
        (float(r["Time"]) - float(avg_rows[0]["timestamp_ms"])) / 1000.0
        for r in sctp_rows
    ]
    throughput = [float(r["Throughput"]) for r in sctp_rows]

    fig, ax = plt.subplots()
    ax.plot(avg_times, bitrates, label="RTP Bitrate (Mbps)")
    ax.plot(sctp_times, throughput, label="SCTP Throughput (Mbps)")

    start_drawn = stop_drawn = False
    for r, t in zip(sctp_rows, sctp_times):
        if r.get("Start") == "1" and not start_drawn:
            ax.axvline(t, color="green", linestyle="--", label="SCTP start")
            start_drawn = True
        elif r.get("Start") == "1":
            ax.axvline(t, color="green", linestyle="--")
        if r.get("Stop") == "1" and not stop_drawn:
            ax.axvline(t, color="red", linestyle="--", label="SCTP stop")
            stop_drawn = True
        elif r.get("Stop") == "1":
            ax.axvline(t, color="red", linestyle="--")

    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Rate (Mbps)")
    ax.legend()
    plt.tight_layout()
    plt.show()
    plt.savefig(args.output)
    print(f"Plot saved to {args.output}")


if __name__ == "__main__":
    main()
