# WebRTC Testbed Platform

A WebRTC testbed for **reproducible, automated media-streaming experiments over
emulated networks**. It is built on the native (C++) WebRTC stack and ships a
`peerconnection_client` example extended for headless operation, metric logging,
and trace-driven network emulation, plus scripts that run full sender/receiver
sessions automatically and analyze the results.

> This repository is a fork of [WebRTC](https://webrtc.googlesource.com/src).
> The upstream stack is unchanged; the additions live in `examples/`,
> `automated_experiment/`, and `analysis/`. See `LICENSE`, `PATENTS`, and
> `AUTHORS` for the upstream license and attribution.

## What you can do with it

- Stream real video (Y4M) between a sender and a receiver over a peer connection.
- Shape the network with a **trace-driven emulator** (`tc`/`netem`) so each run
  replays a recorded bandwidth profile.
- Run **batch experiments** across many network traces and traffic configurations
  with a single command, headless.
- Collect per-frame and aggregate metrics (frame rate, bitrate, latency, …) and
  generate comparison plots.

## Repository layout

| Path | Purpose |
|------|---------|
| `examples/peerconnection/client/` | Extended `peerconnection_client` (Y4M source, headless mode, MP4 recording, stats logging, WebSocket signalling) |
| `automated_experiment/` | Experiment drivers, network emulator, traces, traffic configs, analysis |
| `analysis/` | Standalone log-analysis and plotting helpers |
| `tools_webrtc/build_libwebsockets.sh` | Builds the libwebsockets dependency (run by a gclient hook) |

## Prerequisites

Experiments target **Linux** (the emulator uses network namespaces and `tc`).

- [`depot_tools`](https://commondatastorage.googleapis.com/chrome-infra-docs/flat/depot_tools/docs/html/depot_tools_tutorial.html) on your `PATH`
- Build/runtime packages:
  ```bash
  sudo apt install cmake pkg-config libssl-dev libcurl4-openssl-dev \
                   libabsl-dev iproute2 pulseaudio python3 python3-pip
  pip3 install pandas numpy matplotlib aiohttp
  ```
  (`libabsl-dev` + `pkg-config` are needed to build the network emulator;
  `cmake`/`libssl-dev`/`libcurl4-openssl-dev` to build libwebsockets.)

## Build

1. Sync dependencies. `gclient sync` fetches everything, including
   `third_party/libwebsockets`, and the `build_libwebsockets` hook compiles it
   automatically:
   ```bash
   gclient sync
   ```
   If the hook was skipped (e.g. `cmake` was missing), build it manually later:
   ```bash
   bash tools_webrtc/build_libwebsockets.sh
   ```

2. Generate the build directory and build the client:
   ```bash
   gn gen out/Default
   ninja -C out/Default peerconnection_client
   ```
   The binary is produced at `out/Default/peerconnection_client`.

## Signalling server

The client uses an AppRTC-style signalling flow: an HTTP `POST /join/<room>` and
`POST /message/<room>/<client>`, plus a WebSocket connection to receive messages.
Point the client at a server with `--server` and `--port` (defaults
`localhost` / `8888`), and pick the URL scheme with `--server_scheme`
(`https`, the default, for an external server on 443; `http` for the bundled
local server, which appends `--port` to the URL).

A **bundled signalling server** now ships at
`automated_experiment/signaling_server.py` (single-file `aiohttp` server; serves
`/join`, `/message`, and the `/ws` WebSocket on one port). `automated_experiment.sh`
manages it automatically: on startup it probes `/healthz` on the chosen port and
**reuses a server that is already running** (and leaves it running on exit);
otherwise it starts one and stops it when the run finishes. So the whole testbed
runs from this repo alone, and you can also leave a server up across many runs.
Pass `--no-local-signaling` to opt out entirely (e.g. to use an external server).
To run it by hand:

```bash
python3 automated_experiment/signaling_server.py --host 0.0.0.0 --port 8888
# then point clients at it with: --server=localhost --port=8888 --server_scheme=http
```

> Note: the separately-built `peerconnection_server` is the *legacy* WebRTC HTTP
> signalling sample and is **not** compatible with this client; use
> `signaling_server.py`.

## Video input

The sender streams a `.y4m` file given via `--y4m_path`. Sample clip:

```bash
mkdir -p automated_experiment/dataset
curl -L -o automated_experiment/dataset/talking_head.y4m \
  "https://www.dropbox.com/scl/fi/73p7xl1pnkh2terwda14i/youtube_walking_talking_head.y4m?rlkey=zrh8bftem90smyzh55b59gee9&dl=1"
```

## Running a single session

Start the **sender first**, then the receiver in another shell, using the same
`room_id`:

```bash
cd automated_experiment
./run_experiment.sh myroom true  --y4m ./dataset/talking_head.y4m   # sender
./run_experiment.sh myroom false                                    # receiver
```

`run_experiment.sh` wraps `peerconnection_client`. Useful flags:
`--min/--max` (playout delay ms), `--y4m PATH`, `--sctp PATH`.

## Running automated experiments (the main workflow)

`automated_experiment/automated_experiment.sh` is the batch driver. For every
network trace × traffic configuration it: converts the trace to an emulator
profile, starts the emulator, launches the sender inside an emulated namespace
and the receiver on the host, waits for the run to finish, and archives the logs.

```bash
cd automated_experiment
sudo ./automated_experiment.sh \
  --experiment-id my_batch \
  --traces-dir ./poc_traces \
  --traffic-dir ./config \
  --y4m ./dataset/talking_head.y4m \
  --server localhost --port 8888
```

Key options (run with `--help` for the full list):

| Option | Meaning | Default |
|--------|---------|---------|
| `--experiment-id ID` | Groups output under `results/ID` | (required) |
| `--traces-dir DIR` | Directory of `*.pitree-trace` files | `poc_traces` |
| `--traffic-dir DIR` | Directory of traffic-config sets | `config` |
| `--interface NAME` | Host interface to shape | auto-detected |
| `--latency-ms N` | Fixed latency applied to traces | `30` |
| `--server` / `--port` | Signalling server | `localhost` / `8888` |
| `--y4m PATH` | Video file for the sender | (none) |

`sudo` is required because the emulator manipulates network namespaces and `tc`.
The network emulator is compiled on demand (`make` in `network_emulation/`).

### Traces

`poc_traces/` holds a few sample bandwidth traces; `traces/` holds the full set.
Each `*.pitree-trace` line is `<time_s> <bandwidth_mbps>`; `convert_trace.py`
turns it into an emulator profile CSV.

### Traffic configurations

A traffic-config *set* is a directory under `config/`. The main file is
`rtp.csv`, which describes the video stream; one line per flow with a header row:

```csv
Traffic name,Protocol,Pattern,File size,Periodicity,Custom traces,Max bitrate,Frame rate,Video file,SLO (ms)
Video,RTP,Video,,,,8000000,30,,
```

Here `Max bitrate` is in bps and `Frame rate` in fps. Provided sets:

| Set | Video |
|-----|-------|
| `video_high` | 8 Mbps, 30 fps |
| `video_low` | 2.5 Mbps, 30 fps |
| `video_with_data` | 5 Mbps video + an optional background data channel (`sctp.csv`) |

A set may also include an `sctp.csv` to send data-channel traffic alongside the
video; the sender receives these as `--rtp_csv` / `--sctp_csv`.

## Analyzing results

```bash
cd automated_experiment
python3 analyze_results.py --results-dir results/my_batch --output-dir plots
```

This reads each run's receiver logs (`average_stats.csv`, `frame_metrics.csv`)
and writes a `summary.csv` plus comparison plots to `plots/`: frame rate,
bitrate, dropped frames, and per-frame transport-latency / jitter CDFs. The
helpers in `../analysis/` operate on individual log files.

## Notes

- Generated artifacts (build outputs, run logs, plots, `third_party/libwebsockets`)
  are git-ignored; only sources and input data are tracked.
- `peerconnection_client` also exposes flags such as `--headless`, `--is_sender`,
  `--record_remote`/`--record_path` (MP4 capture), and `--force_fieldtrials`.
