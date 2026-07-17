# Automated experiments

This directory contains everything needed to run trace-driven WebRTC experiments
over an emulated network and to analyze the results. For build instructions and
the overall picture, see the [top-level README](../README.md); this document
covers the contents of this directory in detail.

## Contents

| Path | Purpose |
|------|---------|
| `automated_experiment.sh` | Batch driver: runs every trace × traffic config |
| `run_experiment.sh` | Launch a single `peerconnection_client` (sender or receiver) |
| `convert_trace.py` | Convert a `*.pitree-trace` into an emulator profile CSV |
| `analyze_results.py` | Aggregate run logs and produce comparison plots |
| `connection_manager.py` | Optional HTTP helper to start/stop clients remotely |
| `network_emulation/` | `tc`/`netem` network emulator (C++, built with `make`) |
| `poc_traces/`, `traces/` | Sample and full bandwidth traces |
| `config/` | Traffic-configuration sets (`<set>/rtp.csv`, `<set>/sctp.csv`) |

## Single session

Start the sender first, then the receiver, using the same `room_id`:

```bash
./run_experiment.sh myroom true  --y4m ./dataset/talking_head.y4m   # sender
./run_experiment.sh myroom false                                    # receiver
```

Options: `--min/--max` (playout delay ms), `--y4m PATH`, `--sctp PATH`.
The signalling server defaults to `localhost`; override it by editing the script
or passing `--server` to `peerconnection_client` directly.

## Batch experiments

`automated_experiment.sh` is the main driver. For each network trace × traffic
configuration it:

1. Converts the trace into an emulator profile CSV (`convert_trace.py`).
2. Starts the network emulator against the host interface (built on demand).
3. Launches the sender inside the emulated namespace (`ip netns exec ns1 …`) and
   the receiver on the host.
4. Waits for the run to finish, then stops the emulator and archives the logs.

```bash
sudo ./automated_experiment.sh \
    --experiment-id my_batch \
    --traces-dir ./poc_traces \
    --traffic-dir ./config \
    --y4m ./dataset/talking_head.y4m \
    --server localhost --port 8888
```

Run `./automated_experiment.sh --help` for the full option list. `sudo` is
required because the emulator manipulates network namespaces and `tc` queues.

### Network emulator

`network_emulation/` builds a small `netem`-based emulator. It installs the
queue once and then issues `tc qdisc replace` to adjust bandwidth/delay (updates
are rate-limited to 250 ms and skipped when parameters are unchanged), which
avoids the brief disconnects caused by tearing the queue down and recreating it.

### Traffic configurations

A traffic-config *set* is a subdirectory of `config/`. The main file is
`rtp.csv`, describing the video stream (one line per flow, with a header):

```csv
Traffic name,Protocol,Pattern,File size,Periodicity,Custom traces,Max bitrate,Frame rate,Video file,SLO (ms)
Video,RTP,Video,,,,8000000,30,,
```

`Max bitrate` is in bps, `Frame rate` in fps. Provided sets:

| Set | Video |
|-----|-------|
| `video_high` | 8 Mbps, 30 fps |
| `video_low` | 2.5 Mbps, 30 fps |
| `video_with_data` | 5 Mbps video + a background data channel (`sctp.csv`) |

A set may also include an `sctp.csv` to send data-channel traffic alongside the
video; these are passed to the sender as `--rtp_csv` / `--sctp_csv`. For SCTP
flows the optional `SLO (ms)` column sets the maximum acceptable delivery delay,
and the client records the observed delay and running satisfaction ratio.

## Output layout

All artifacts are written under `results/<experiment-id>/`:

```
results/<experiment-id>/<traffic-set>/<trace>/
├── profiles/        # emulator profile CSVs
├── webrtc_logs/     # per-role client logs (frame/aggregate metrics)
├── emulator_logs/   # raw emulator output
└── stdout/          # sender/receiver/emulator stdout
```

## Analysis

```bash
python3 analyze_results.py --results-dir results/my_batch --output-dir plots
```

For each run it reads the receiver's `average_stats.csv` and `frame_metrics.csv`
and writes `summary.csv` plus comparison plots: frame rate, bitrate, dropped
frames, and per-frame transport-latency / jitter CDFs across traces and configs.
