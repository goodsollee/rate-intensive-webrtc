#!/usr/bin/env python3
import pandas as pd
import numpy as np
from pathlib import Path
import argparse
import matplotlib.pyplot as plt

def print_keyframe_intervals(frame_df):
    """
    Print all intervals between consecutive key-frames based on
    estimated_first_packet_departure column (ms).
    """
    col = "estimated_first_packet_departure"
    if col not in frame_df.columns:
        print(f"[Keyframe] Column '{col}' not found.")
        return

    # 1) Filter keyframes
    if 'is_keyframe' in frame_df.columns:
        kf_df = frame_df[frame_df['is_keyframe'] == 1].copy()
    elif 'frame_type' in frame_df.columns:
        kf_df = frame_df[frame_df['frame_type'].astype(str)
                         .str.contains(r'\bkey\b', case=False)].copy()
    else:
        print("[Keyframe] Neither 'is_keyframe' nor 'frame_type' found in CSV.")
        return

    if len(kf_df) < 2:
        print("[Keyframe] Not enough key-frames to compute intervals.")
        return

    # 2) Sort by time and compute inter-frame gap (ms)
    kf_df = kf_df.sort_values(col)
    times = kf_df[col].astype(np.int64).to_numpy()
    intervals = np.diff(times)

    # 3) Output
    print(f"\n=== Key-frame {col} values (ms) ===")
    for t in times:
        print(t)

    print(f"\n=== Key-frame intervals based on {col} (ms) ===")
    for i, d in enumerate(intervals, 1):
        print(f"{i}: {d} ms")

    # 4) Statistics
    print("\nKey-frame interval stats (ms):")
    print(f"count   : {len(intervals)}")
    print(f"mean    : {np.mean(intervals):.2f}")
    print(f"median  : {np.median(intervals):.2f}")
    print(f"p05/p95 : {np.percentile(intervals, 5):.2f} / {np.percentile(intervals, 95):.2f}")


def analyze_webrtc_metrics(folder_name):
    """
    Analyze WebRTC metrics from given folder containing frame_metrics.csv and average_stats.csv
    """
    # Construct file paths
    base_path = Path(folder_name)
    frame_metrics_path = base_path / "receiver" / "frame_metrics.csv"
    avg_stats_path = base_path / "receiver" / "average_stats.csv"

    # Check if files exist
    if not frame_metrics_path.exists():
        raise FileNotFoundError(f"Frame metrics file not found at {frame_metrics_path}")
    if not avg_stats_path.exists():
        raise FileNotFoundError(f"Average stats file not found at {avg_stats_path}")

    # Read CSVs
    frame_df = pd.read_csv(frame_metrics_path)
    stats_df = pd.read_csv(avg_stats_path)

    # Overall average bitrate stats
    bitrate_series = stats_df['overall_avg_bitrates'].dropna()
    bitrate_stats = {
        'mean'  : bitrate_series.mean(),
        'median': bitrate_series.median(),
        'std'   : bitrate_series.std(),
        '95th'  : np.percentile(bitrate_series, 95),
        '99th'  : np.percentile(bitrate_series, 99),
        '99.9th': np.percentile(bitrate_series, 99.9)
    }

    # Estimated network ms stats
    network_series = frame_df['estimated_network_ms'].dropna()
    network_stats = {
        'mean'  : network_series.mean(),
        'median': network_series.median(),
        'std'   : network_series.std(),
        '95th'  : np.percentile(network_series, 95),
        '99th'  : np.percentile(network_series, 99),
        '99.9th': np.percentile(network_series, 99.9)
    }

    # Framerate stats
    framerate = stats_df['framerate'].dropna()
    framerate_stats = {
        'mean'  : framerate.mean(),
        'median': framerate.median(),
        'std'   : framerate.std(),
        'p0.1'  : np.percentile(framerate, 0.1),
        'p01'   : np.percentile(framerate, 1),
        'p05'   : np.percentile(framerate, 5)
    }


    # Frame-level jitter stats
    frame_df['rtp_ms'] = frame_df['rtp_timestamp'] / 90
    frame_df['departure_time'] = frame_df['rtp_ms'] + frame_df['encode_ms']
    frame_df['departure_diff'] = frame_df['departure_time'].diff()
    frame_df['jitter'] = frame_df['inter_frame_delay_ms'] - frame_df['departure_diff']
    jitter_clean = frame_df['jitter'].replace([np.inf, -np.inf], np.nan).dropna()

    if len(jitter_clean) > 0:
        jitter_stats = {
            'mean'  : jitter_clean.mean(),
            'median': jitter_clean.median(),
            '95th'  : np.percentile(jitter_clean, 95),
            '99th'  : np.percentile(jitter_clean, 99),
            '99.9th': np.percentile(jitter_clean, 99.9)
        }
    else:
        jitter_stats = {k: np.nan for k in ['mean', 'median', '95th', '99th', '99.9th']}

    # Display
    print(f"\nTotal frames: {len(frame_df)}")
    print(f"Valid jitter measurements: {len(jitter_clean)}")

    print(f"\nAnalysis Results for {folder_name}:")
    print("-" * 50)

    print("\nOverall Average Bitrate Statistics (bps):")
    for k, v in bitrate_stats.items():
        print(f"{k.ljust(10)}: {v:,.2f}")

    print("\nEstimated Network MS Statistics:")
    for k, v in network_stats.items():
        print(f"{k.ljust(10)}: {v:.2f}")

    print("\nFramerate Statistics (fps):")
    for k, v in framerate_stats.items():
        print(f"{k.ljust(10)}: {v:.2f}")

    print("\nFrame-level Jitter Statistics (ms):")
    for k, v in jitter_stats.items():
        print(f"{k.ljust(10)}: {v:.2f}")

    
    df_plot = frame_df.copy()
    df_plot['relative_time_s'] = (df_plot['timestamp'] - df_plot['timestamp'].min()) / 1000.0

    plt.figure(figsize=(8, 5))
    plt.plot(df_plot['relative_time_s'], df_plot['estimated_network_ms'], marker='o')
    plt.xlabel("Time (s)")
    plt.ylabel("Estimated Network Delay (ms)")
    plt.title("Estimated Network Delay vs Time")
    plt.grid(True)
    plt.tight_layout()
    plt.show()

    plt.figure(figsize=(8, 5))
    plt.plot(df_plot['relative_time_s'], df_plot['encoded_size'], marker='o')
    plt.xlabel("Time (s)")
    plt.ylabel("Encoded Size (bytes)")
    plt.title("Encoded Size vs Time")
    plt.grid(True)
    plt.tight_layout()
    plt.show()

    if 'timestamp' in stats_df.columns:
        stats_df['relative_time_s'] = (
            stats_df['timestamp'] - stats_df['timestamp'].min()) / 1000.0
        x_fps = stats_df['relative_time_s']
    else:
        x_fps = stats_df.index  # each row is one stats window

    plt.figure(figsize=(8, 5))
    plt.plot(x_fps, stats_df['framerate'], marker='o')
    plt.xlabel("Time (s)" if 'timestamp' in stats_df.columns else "Stats window")
    plt.ylabel("Framerate (fps)")
    plt.title("Framerate vs Time")
    plt.grid(True)
    plt.tight_layout()
    plt.show()

    print_keyframe_intervals(frame_df)


def main():
    parser = argparse.ArgumentParser(description='Analyze WebRTC metrics from log files.')
    parser.add_argument('folder', help='Path to the WebRTC logs folder')
    args = parser.parse_args()

    try:
        analyze_webrtc_metrics(args.folder)
    except Exception as e:
        print(f"Error: {e}")
        exit(1)

if __name__ == "__main__":
    main()
