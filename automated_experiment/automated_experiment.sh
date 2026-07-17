#!/usr/bin/env bash
set -euo pipefail

################################################################################
# Argument parsing
################################################################################
usage() {
    cat <<USAGE
Usage: $0 --experiment-id ID [options]

Required arguments:
  --experiment-id ID           Unique identifier used to group log output.

Optional arguments:
  --traces-dir DIR             Directory containing *.pitree-trace files
                               [default: <repo>/automated_experiment/poc_traces]
  --traffic-dir DIR            Directory containing traffic config sets (subdirectories)
                               [default: <repo>/automated_experiment/config]
  --traffic-config PATH        Single traffic config set directory (overrides --traffic-dir)
  --output-dir DIR             Root directory where experiment artifacts are stored
                               [default: <repo>/automated_experiment/results]
  --latency-ms N               Fixed latency (ms) applied when converting traces [default: 30]
  --interface NAME             Physical interface to shape (auto-detected if omitted)
  --namespace NAME             Network namespace created by the emulator [default: ns1]
  --ns-interface NAME          Interface name inside the namespace [default: veth_ns]
  --server HOST                Signalling server host [default: 192.168.100.1,
                               the veth host IP reachable from the sender's netns]
  --port PORT                  Signalling server port [default: 8888]
  --signaling-scheme SCHEME    http (bundled local server) or https [default: http]
  --no-local-signaling         Do not auto-start the bundled signalling server
                               (point --server at an external server instead)
  --sender-headless BOOL       Run sender headless? [default: true]
  --receiver-headless BOOL     Run receiver headless? [default: false]
  --y4m PATH                   Optional Y4M file for the sender
  -h, --help                   Show this message
USAGE
}

if pgrep -f peerconnection_client >/dev/null 2>&1; then
    echo "Found running peerconnection_client processes — killing them..."
    sudo pkill -9 -f peerconnection_client >/dev/null 2>&1 || true
else
    echo "No peerconnection_client processes found."
fi


SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)

# peerconnection_client always calls gtk_init (even in --headless), which needs an
# X display. On a headless host, run it under a virtual display via xvfb-run.
if [[ -z "${DISPLAY:-}" ]] && command -v xvfb-run >/dev/null 2>&1; then
    XVFB=(xvfb-run -a)
else
    XVFB=()
fi

RUN_START_TS="$(date +%Y%m%dT%H%M%S)"

TRACES_DIR="$SCRIPT_DIR/poc_traces"
TRAFFIC_DIR="$SCRIPT_DIR/config"
TRAFFIC_CONFIG=""
OUTPUT_DIR="$SCRIPT_DIR/results"
LATENCY_MS=30
INTERFACE_NAME=""
NS_NAME="ns1"
NS_INTERFACE="veth_ns"
SERVER_HOST="192.168.100.1"
SERVER_PORT=8888
SIGNALING_SCHEME="http"
START_LOCAL_SIGNALING="true"
SENDER_HEADLESS="true"
RECEIVER_HEADLESS="true"
EXPERIMENT_ID=""
Y4M_PATH=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --experiment-id) EXPERIMENT_ID="$2"; shift 2 ;;
        --traces-dir) TRACES_DIR="$2"; shift 2 ;;
        --traffic-dir) TRAFFIC_DIR="$2"; shift 2 ;;
        --traffic-config) TRAFFIC_CONFIG="$2"; shift 2 ;;
        --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
        --latency-ms) LATENCY_MS="$2"; shift 2 ;;
        --interface) INTERFACE_NAME="$2"; shift 2 ;;
        --namespace) NS_NAME="$2"; shift 2 ;;
        --ns-interface) NS_INTERFACE="$2"; shift 2 ;;
        --server) SERVER_HOST="$2"; shift 2 ;;
        --port) SERVER_PORT="$2"; shift 2 ;;
        --signaling-scheme) SIGNALING_SCHEME="$2"; shift 2 ;;
        --no-local-signaling) START_LOCAL_SIGNALING="false"; shift ;;
        --sender-headless) SENDER_HEADLESS="$2"; shift 2 ;;
        --receiver-headless) RECEIVER_HEADLESS="$2"; shift 2 ;;
        --y4m) Y4M_PATH="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown argument: $1"; usage; exit 1 ;;
    esac
done

if [[ -z "$EXPERIMENT_ID" ]]; then
    echo "Error: --experiment-id is required" >&2
    usage
    exit 1
fi

if [[ ! -d "$TRACES_DIR" ]]; then
    echo "Error: traces directory not found: $TRACES_DIR" >&2
    exit 1
fi

# Build list of traffic configs
TRAFFIC_CONFIGS=()
if [[ -n "$TRAFFIC_CONFIG" ]]; then
    # Single config set directory specified
    if [[ ! -d "$TRAFFIC_CONFIG" ]]; then
        echo "Error: traffic config directory not found: $TRAFFIC_CONFIG" >&2
        exit 1
    fi
    TRAFFIC_CONFIGS=("$TRAFFIC_CONFIG")
else
    # Discover all traffic config directories in traffic directory
    if [[ ! -d "$TRAFFIC_DIR" ]]; then
        echo "Error: traffic directory not found: $TRAFFIC_DIR" >&2
        exit 1
    fi
    while IFS= read -r -d '' config_dir; do
        TRAFFIC_CONFIGS+=("$config_dir")
    done < <(find "$TRAFFIC_DIR" -mindepth 1 -maxdepth 1 -type d -print0 | sort -z)

    if [[ ${#TRAFFIC_CONFIGS[@]} -eq 0 ]]; then
        echo "Error: no traffic config directories found in $TRAFFIC_DIR" >&2
        exit 1
    fi
    echo "Found ${#TRAFFIC_CONFIGS[@]} traffic config set(s) in $TRAFFIC_DIR"
fi

if [[ -n "$Y4M_PATH" && ! -f "$Y4M_PATH" ]]; then
    echo "Error: Y4M file not found: $Y4M_PATH" >&2
    exit 1
fi

if [[ -z "$INTERFACE_NAME" ]]; then
    INTERFACE_NAME=$(ip route get 8.8.8.8 2>/dev/null | awk '{for(i=1;i<=NF;i++) if($i=="dev") {print $(i+1); exit}}')
    if [[ -z "$INTERFACE_NAME" ]]; then
        echo "Error: failed to auto-detect interface. Pass --interface explicitly." >&2
        exit 1
    fi
fi


NETWORK_EMULATOR_BIN="$SCRIPT_DIR/network_emulation/network_emulator"
if [[ ! -x "$NETWORK_EMULATOR_BIN" ]]; then
    echo "Building network emulator..."
    (cd "$SCRIPT_DIR/network_emulation" && make)
fi

EMULATOR_SUPPORTS_BANDWIDTH_LOG=false
EMULATOR_HELP_OUTPUT="$("$NETWORK_EMULATOR_BIN" --helpshort 2>&1 || true)"
if grep -q -- '--bandwidth_log_path' <<<"$EMULATOR_HELP_OUTPUT"; then
    EMULATOR_SUPPORTS_BANDWIDTH_LOG=true
else
    echo "Info: network_emulator does not support --bandwidth_log_path; bandwidth CSV logging disabled."
fi


if [[ ! -x "$REPO_ROOT/out/Default/peerconnection_client" ]]; then
    echo "Error: peerconnection_client binary not found at $REPO_ROOT/out/Default/peerconnection_client" >&2
    exit 1
fi

convert_trace() {
    local trace_file="$1"
    local output_file="$2"
    python3 "$SCRIPT_DIR/convert_trace.py" "$trace_file" "$output_file" --latency-ms "$LATENCY_MS"
}

cleanup_all_processes() {
    echo "=== COMPREHENSIVE CLEANUP ==="

    # 1. Kill all peerconnection_client processes (both in and out of namespace)
    echo "  Killing all peerconnection_client processes..."
    sudo pkill -9 -f peerconnection_client >/dev/null 2>&1 || true

    # Also kill in namespace specifically
    sudo ip netns exec "$NS_NAME" pkill -9 -f peerconnection_client >/dev/null 2>&1 || true

    # 2. Kill any network_emulator processes
    echo "  Killing network_emulator processes..."
    sudo pkill -9 -f network_emulator >/dev/null 2>&1 || true

    # 3. Kill PulseAudio in namespace and host
    echo "  Stopping PulseAudio..."
    sudo ip netns exec "$NS_NAME" pulseaudio --kill >/dev/null 2>&1 || true
    sudo pulseaudio --kill >/dev/null 2>&1 || true

    # 4. Clean up WebRTC-related files and caches
    echo "  Cleaning WebRTC state..."
    # Remove any stale WebRTC cache/state files
    rm -rf /tmp/.org.chromium.Chromium.* >/dev/null 2>&1 || true
    rm -rf /tmp/peerconnection_client_* >/dev/null 2>&1 || true
    rm -f /tmp/emulator_ready.signal >/dev/null 2>&1 || true

    # Clean namespace-specific temp files
    sudo ip netns exec "$NS_NAME" bash -c 'rm -rf /tmp/.org.chromium.Chromium.* /tmp/peerconnection_client_*' >/dev/null 2>&1 || true

    # 5. Wait for processes to actually die
    echo "  Waiting for process cleanup..."
    local wait_count=0
    while pgrep -f peerconnection_client >/dev/null 2>&1; do
        sleep 0.5
        wait_count=$((wait_count + 1))
        if [[ $wait_count -gt 10 ]]; then
            echo "  WARNING: Some processes still alive after 5 seconds"
            pgrep -af peerconnection_client || true
            break
        fi
    done

    # 6. Kill any lingering curl processes (stale HTTP connections)
    echo "  Cleaning up stale HTTP connections..."
    sudo pkill -9 curl >/dev/null 2>&1 || true
    sudo ip netns exec "$NS_NAME" pkill -9 curl >/dev/null 2>&1 || true

    # 7. Extra sleep to ensure ports are freed and connections cleaned
    sleep 3

    echo "✓ Cleanup complete"
}

run_single_trace() {
    local trace_path="$1"
    local sctp_config="$2"
    local rtp_config="$3"
    local traffic_label="$4"
    local attempt_num="${5:-1}"  # Accept attempt number as 5th parameter
    local trace_file
    trace_file=$(basename "$trace_path")
    local trace_name="${trace_file%.pitree-trace}"

    local traffic_base="${traffic_label:-traffic}"
    traffic_base="${traffic_base// /_}"

    # Generate a fresh timestamp for each attempt
    local current_ts="$(date +%Y%m%dT%H%M%S)"
    local room_id="${current_ts}_${trace_name}_${traffic_base}_attempt${attempt_num}"

    echo -e "\n=== Running trace: $trace_name (room_id=${room_id}) ==="

    # ===== CRITICAL: Clean up all processes before starting =====
    cleanup_all_processes
    # ============================================================

    local profile_csv="$PROFILES_DIR/${trace_name}.csv"
    convert_trace "$trace_path" "$profile_csv"

    local run_stdout_dir="$STDOUT_DIR/$trace_name"
    mkdir -p "$run_stdout_dir"

    local emulator_stdout="$run_stdout_dir/network_emulator.log"
    local bandwidth_csv="$EMULATOR_LOG_DIR/${trace_name}_bandwidth.csv"

    # ===== Remove the signal file first =====
    local signal_file="/tmp/emulator_ready.signal"
    rm -f "$signal_file"
    # ============================

    # Create FIFO
    local fifo_path="$run_stdout_dir/emulator.fifo"
    [[ -p "$fifo_path" ]] || mkfifo "$fifo_path"
    sudo chmod 666 "$fifo_path"

    # Open FIFO on fd 3
    exec 3<>"$fifo_path"

    # Emulator command
    local emulator_cmd=(
        sudo "$NETWORK_EMULATOR_BIN"
        "--profile_path=$profile_csv"
        "--interface_name=$INTERFACE_NAME"
    )

    if [[ "$EMULATOR_SUPPORTS_BANDWIDTH_LOG" == "true" ]]; then
        emulator_cmd+=("--bandwidth_log_path=$bandwidth_csv")
    fi

    echo "Starting emulator..."
    
    # Run emulator
    "${emulator_cmd[@]}" <&3 >"$emulator_stdout" 2>&1 &
    local emulator_pid=$!

    sleep 1
    
    # Check process
    if ! kill -0 "$emulator_pid" 2>/dev/null; then
        echo "ERROR: Emulator died!" >&2
        exec 3>&-
        return 1
    fi
    echo "✓ Emulator started (PID: $emulator_pid)"

    echo "$emulator_pid" > "$run_stdout_dir/emulator.pid"

    # Wait for the signal file (do not remove!)
    echo "Waiting for emulator initialization..."
    local wait_seconds=0
    while [[ ! -f "$signal_file" ]]; do
        # Debug: check file existence
        if [[ $((wait_seconds % 5)) -eq 0 ]]; then
            ls -la "$signal_file" 2>&1 | sed 's/^/  DEBUG: /'
        fi
        
        if ! kill -0 "$emulator_pid" 2>/dev/null; then
            echo "ERROR: Emulator exited!" >&2
            tail -n 50 "$emulator_stdout" >&2
            exec 3>&-
            return 1
        fi
        
        sleep 1
        wait_seconds=$((wait_seconds + 1))

        if [[ $((wait_seconds % 5)) -eq 0 ]]; then
            echo "  Still waiting... (${wait_seconds}s)"
            tail -n 3 "$emulator_stdout" | sed 's/^/    /'
        fi

        if [[ $wait_seconds -gt 30 ]]; then
            echo "ERROR: Timeout!" >&2
            exec 3>&-
            return 1
        fi
    done

    echo "✓ Emulator ready! (${wait_seconds}s)"
    rm -f "$signal_file"  # remove after use

    echo "start" >&3  # write to fd 3

    # Sender/Receiver setup
    local sender_log="$run_stdout_dir/sender.log"
    local receiver_log="$run_stdout_dir/receiver.log"

    local common_args=(
        --room_id="$room_id"
        --log_root="$LOG_ROOT"
        --server="$SERVER_HOST"
        --port="$SERVER_PORT"
        --server_scheme="$SIGNALING_SCHEME"
        --autoconnect=true
    )

    if [[ -n "$sctp_config" ]]; then
        common_args+=("--sctp_csv=$sctp_config")
    fi
    if [[ -n "$rtp_config" ]]; then
        common_args+=("--rtp_csv=$rtp_config")
    fi

    local sender_args=(
        sudo ip netns exec "$NS_NAME" "${XVFB[@]}" "$REPO_ROOT/out/Default/peerconnection_client"
        "${common_args[@]}"
        --is_sender=true
        --autocall=true
        --headless="$SENDER_HEADLESS"
    )
    if [[ -n "$Y4M_PATH" ]]; then
        sender_args+=("--y4m_path=$Y4M_PATH")
    fi

    local receiver_args=(
        sudo "${XVFB[@]}" "$REPO_ROOT/out/Default/peerconnection_client"
        "${common_args[@]}"
        --is_sender=false
        --headless="$RECEIVER_HEADLESS"
    )

    # Start PulseAudio and launch clients
    echo "Starting sender and receiver..."

    # Start PulseAudio with a clean state
    sudo ip netns exec "$NS_NAME" pulseaudio --kill >/dev/null 2>&1 || true
    sleep 1
    sudo ip netns exec "$NS_NAME" pulseaudio --start >/dev/null 2>&1 || true

    # Start sender FIRST and wait for it to be ready
    echo "  Starting sender..."
    "${sender_args[@]}" > "$sender_log" 2>&1 &
    local sender_pid=$!
    echo "  Sender PID: $sender_pid"

    # Wait for sender to complete signaling server registration
    local sender_wait=0
    while [[ $sender_wait -lt 10 ]]; do
        if ! kill -0 "$sender_pid" 2>/dev/null; then
            echo "ERROR: Sender died during startup!" >&2
            if [[ -f "$sender_log" ]]; then
                tail -n 50 "$sender_log" >&2
            fi
            exec 3>&-
            return 1
        fi

        # Check if sender has completed WebSocket/HTTP setup
        if [[ -f "$sender_log" ]] && grep -q "lws_create_context" "$sender_log" 2>/dev/null; then
            echo "  ✓ Sender initialized (${sender_wait}s)"
            break
        fi

        sleep 1
        sender_wait=$((sender_wait + 1))
    done

    # Extra delay to ensure sender is fully ready
    sleep 5

    # Now start receiver
    echo "  Starting receiver..."
    sudo pulseaudio --kill >/dev/null 2>&1 || true
    sleep 1
    sudo pulseaudio --start >/dev/null 2>&1 || true

    "${receiver_args[@]}" > "$receiver_log" 2>&1 &
    local receiver_pid=$!
    echo "  Receiver PID: $receiver_pid"

    # Wait for traffic detection on the receiver (RTP or SCTP)
    echo "Waiting for traffic in receiver.log..."
    local traffic_wait=0
    # Check for either RTP video traffic OR SCTP data channel traffic
    while [[ ! -f "$receiver_log" || -z $(grep -E "(Elapsed time|Frame rate|Bitrate|\[SCTP\]\[FILE\]\[Receiver\])" "$receiver_log" 2>/dev/null) ]]; do
        if ! kill -0 "$receiver_pid" 2>/dev/null; then
            echo "ERROR: Receiver exited before traffic started!" >&2
            if [[ -f "$receiver_log" ]]; then
                echo "=== Receiver log ===" >&2
                tail -n 50 "$receiver_log" >&2
            fi
            kill "$sender_pid" 2>/dev/null || true
            exec 3>&-
            return 1
        fi

        # Also check sender is still alive
        if ! kill -0 "$sender_pid" 2>/dev/null; then
            echo "ERROR: Sender died while waiting for traffic!" >&2
            if [[ -f "$sender_log" ]]; then
                echo "=== Sender log ===" >&2
                tail -n 50 "$sender_log" >&2
            fi
            kill "$receiver_pid" 2>/dev/null || true
            exec 3>&-
            return 1
        fi

        sleep 1
        traffic_wait=$((traffic_wait + 1))

        if [[ $((traffic_wait % 10)) -eq 0 ]]; then
            echo "  Still waiting for traffic... (${traffic_wait}s)"
        fi

        if [[ $traffic_wait -gt 30 ]]; then
            echo "ERROR: Timeout waiting for traffic (30s)!" >&2
            if [[ -f "$receiver_log" ]]; then
                echo "=== Receiver log ===" >&2
                tail -n 50 "$receiver_log" >&2
            fi
            if [[ -f "$sender_log" ]]; then
                echo "=== Sender log ===" >&2
                tail -n 50 "$sender_log" >&2
            fi
            kill "$sender_pid" 2>/dev/null || true
            kill "$receiver_pid" 2>/dev/null || true
            exec 3>&-
            return 1
        fi
    done
    echo "✓ Traffic detected! (${traffic_wait}s)"

    # Send the start command to the emulator after traffic begins
    echo -e "\n=== STARTING BANDWIDTH EMULATION ==="
    #sleep 1
    #echo "start" >&3  # write to fd 3
    echo "✓ Start signal sent to emulator for trace: $trace_name"

    # Wait for the emulator to finish
    echo "Waiting for network emulator to complete..."
    local exit_code=0
    wait "$emulator_pid" || exit_code=$?
    echo "✓ Network emulator finished (exit code: $exit_code)"

    # Close fd
    exec 3>&-

    # Clean up sender/receiver
    echo -e "\n=== CLEANING UP ==="
    echo "Stopping sender and receiver..."

    if kill -0 "$sender_pid" 2>/dev/null; then
        echo "  Sending SIGTERM to sender (PID: $sender_pid)"
        sudo kill -TERM "$sender_pid" 2>/dev/null || true
    fi

    if kill -0 "$receiver_pid" 2>/dev/null; then
        echo "  Sending SIGTERM to receiver (PID: $receiver_pid)"
        sudo kill -TERM "$receiver_pid" 2>/dev/null || true
    fi

    # Wait up to 5s for graceful shutdown
    local cleanup_wait=0
    while [[ $cleanup_wait -lt 5 ]]; do
        local sender_alive=0
        local receiver_alive=0

        kill -0 "$sender_pid" 2>/dev/null && sender_alive=1
        kill -0 "$receiver_pid" 2>/dev/null && receiver_alive=1

        if [[ $sender_alive -eq 0 && $receiver_alive -eq 0 ]]; then
            echo "✓ Sender and receiver exited gracefully"
            break
        fi

        sleep 1
        cleanup_wait=$((cleanup_wait + 1))
    done

    # Force kill
    if kill -0 "$sender_pid" 2>/dev/null; then
        echo "  Sender did not exit gracefully, sending SIGKILL"
        sudo kill -9 "$sender_pid" 2>/dev/null || true
    fi

    if kill -0 "$receiver_pid" 2>/dev/null; then
        echo "  Receiver did not exit gracefully, sending SIGKILL"
        sudo kill -9 "$receiver_pid" 2>/dev/null || true
    fi

    # Wait for processes to terminate
    wait "$sender_pid" 2>/dev/null || true
    wait "$receiver_pid" 2>/dev/null || true

    # VERIFY processes are actually dead
    local verify_wait=0
    while [[ $verify_wait -lt 10 ]]; do
        local still_alive=0
        if ps -p "$sender_pid" >/dev/null 2>&1; then
            still_alive=1
            echo "  WARNING: Sender still alive after SIGKILL"
        fi
        if ps -p "$receiver_pid" >/dev/null 2>&1; then
            still_alive=1
            echo "  WARNING: Receiver still alive after SIGKILL"
        fi

        if [[ $still_alive -eq 0 ]]; then
            echo "✓ Verified: All processes terminated"
            break
        fi

        sleep 0.5
        verify_wait=$((verify_wait + 1))
    done

    # Kill any orphaned peerconnection_client processes
    if pgrep -f peerconnection_client >/dev/null 2>&1; then
        echo "  WARNING: Found orphaned peerconnection_client processes, cleaning up..."
        sudo pkill -9 -f peerconnection_client >/dev/null 2>&1 || true
        sudo ip netns exec "$NS_NAME" pkill -9 -f peerconnection_client >/dev/null 2>&1 || true
    fi

    # Stop PulseAudio to release resources
    sudo ip netns exec "$NS_NAME" pulseaudio --kill >/dev/null 2>&1 || true
    sudo pulseaudio --kill >/dev/null 2>&1 || true

    # Copy emulator log
    if [[ -f "$emulator_stdout" ]]; then
        cp "$emulator_stdout" "$EMULATOR_LOG_DIR/${trace_name}.log"
    fi

    # Validate that receiver logs were created properly
    echo "Validating experiment results..."
    local validation_failed=false
    local validation_msg=""

    # Find the receiver directory
    local receiver_dir=""
    for ts_dir in "$LOG_ROOT"/*; do
        if [[ -d "$ts_dir" ]]; then
            for session_dir in "$ts_dir"/*; do
                if [[ -d "$session_dir/receiver" ]]; then
                    receiver_dir="$session_dir/receiver"
                    break 2
                fi
            done
        fi
    done

    if [[ -z "$receiver_dir" ]]; then
        validation_failed=true
        validation_msg="No receiver directory found in $LOG_ROOT"
    else
        # Check if average_stats.csv exists and has data (more than just header)
        local avg_stats_file="$receiver_dir/average_stats.csv"
        if [[ ! -f "$avg_stats_file" ]]; then
            validation_failed=true
            validation_msg="Missing average_stats.csv"
        else
            local line_count=$(wc -l < "$avg_stats_file")
            if [[ $line_count -le 1 ]]; then
                validation_failed=true
                validation_msg="average_stats.csv has no data rows (only $line_count lines)"
            fi
        fi

        # Check for crash indicators in receiver log
        if [[ -f "$receiver_log" ]]; then
            if grep -q -E "(malloc.*corrupt|segmentation fault|core dumped|assertion.*failed)" "$receiver_log" 2>/dev/null; then
                validation_failed=true
                validation_msg="Receiver crashed (detected error in log)"
            fi
        fi

        # Check for connection/signaling failures
        if [[ -f "$receiver_log" ]] && [[ -f "$sender_log" ]]; then
            # Check for timeout in sender log (HTTP connection issue)
            if grep -q "Operation timed out" "$sender_log" 2>/dev/null; then
                validation_failed=true
                validation_msg="Sender HTTP connection timeout (stale connection or network issue)"
            fi

            # Check for WebRTC connection failures
            if grep -q -E "(Failed to set|Failed to create|Error.*ICE|Error.*DTLS)" "$receiver_log" 2>/dev/null || \
               grep -q -E "(Failed to set|Failed to create|Error.*ICE|Error.*DTLS)" "$sender_log" 2>/dev/null; then
                validation_failed=true
                validation_msg="WebRTC connection establishment failed"
            fi
        fi
    fi

    if [[ "$validation_failed" == "true" ]]; then
        echo "✗ Validation failed: $validation_msg"
        if [[ -f "$receiver_log" ]]; then
            echo "=== Last 30 lines of receiver log ===" >&2
            tail -n 30 "$receiver_log" >&2
        fi
        if [[ -f "$sender_log" ]]; then
            echo "=== Last 30 lines of sender log ===" >&2
            tail -n 30 "$sender_log" >&2
        fi
        return 1
    fi

    echo "✓ Trace $trace_name completed successfully"
    echo "  Logs stored at: $LOG_ROOT"

    sleep 3
    return 0
}

# ---- Bundled local signalling server ----------------------------------------
# The sender runs inside netns "$NS_NAME" and reaches the host via 192.168.100.1
# (veth_host); the receiver runs on the host. A server bound to 0.0.0.0:$SERVER_PORT
# is reachable from both, so --server defaults to 192.168.100.1.
SIGNALING_PID=""
SIGNALING_STARTED_BY_US="false"

# True if a signalling server is already answering on the local port.
signaling_is_up() {
    curl -s -m 2 -o /dev/null "http://127.0.0.1:$SERVER_PORT/healthz"
}

stop_local_signaling() {
    # Only stop a server we started ourselves; leave a pre-existing one running.
    if [[ "$SIGNALING_STARTED_BY_US" == "true" && -n "$SIGNALING_PID" ]] \
         && kill -0 "$SIGNALING_PID" 2>/dev/null; then
        echo "Stopping local signalling server (PID $SIGNALING_PID)..."
        kill "$SIGNALING_PID" 2>/dev/null || true
        for _ in $(seq 1 10); do
            kill -0 "$SIGNALING_PID" 2>/dev/null || break
            sleep 0.3
        done
        kill -9 "$SIGNALING_PID" 2>/dev/null || true
        wait "$SIGNALING_PID" 2>/dev/null || true
    fi
}
trap stop_local_signaling EXIT

if [[ "$START_LOCAL_SIGNALING" == "true" ]]; then
    if signaling_is_up; then
        # Reuse a server someone already started (e.g. left running across runs).
        echo "✓ Signalling server already running on port $SERVER_PORT — reusing it (won't stop it)."
    else
        SIGNALING_LOG="$SCRIPT_DIR/signaling_server.log"
        echo "Starting bundled signalling server on 0.0.0.0:$SERVER_PORT (log: $SIGNALING_LOG)"
        python3 "$SCRIPT_DIR/signaling_server.py" --host 0.0.0.0 --port "$SERVER_PORT" \
            > "$SIGNALING_LOG" 2>&1 &
        SIGNALING_PID=$!
        SIGNALING_STARTED_BY_US="true"
        signaling_ready="false"
        for _ in $(seq 1 20); do
            if ! kill -0 "$SIGNALING_PID" 2>/dev/null; then
                echo "ERROR: signalling server exited during startup; see $SIGNALING_LOG" >&2
                tail -n 20 "$SIGNALING_LOG" >&2 || true
                exit 1
            fi
            if signaling_is_up; then signaling_ready="true"; break; fi
            sleep 0.5
        done
        if [[ "$signaling_ready" != "true" ]]; then
            echo "ERROR: signalling server did not become ready; see $SIGNALING_LOG" >&2
            exit 1
        fi
        echo "  ✓ signalling server ready (PID $SIGNALING_PID)"
    fi
    echo "  clients use ${SIGNALING_SCHEME}://${SERVER_HOST}:${SERVER_PORT}"
fi

# Create main experiment root
MAIN_EXPERIMENT_ROOT=$(mkdir -p "$OUTPUT_DIR" && cd "$OUTPUT_DIR" && pwd)/"$EXPERIMENT_ID"
mkdir -p "$MAIN_EXPERIMENT_ROOT"

TOTAL_TRACE_COUNT=0
TOTAL_TRAFFIC_CONFIGS=${#TRAFFIC_CONFIGS[@]}

for traffic_config_dir in "${TRAFFIC_CONFIGS[@]}"; do
    # Extract a unique name for this traffic config set
    traffic_name=$(basename "$traffic_config_dir")

    echo -e "\n========================================="
    echo "Starting experiments with traffic config set: $traffic_name"
    echo "Config path: $traffic_config_dir"
    echo "=========================================\n"

    # Create traffic config subdirectory under main experiment root
    TRAFFIC_ROOT="$MAIN_EXPERIMENT_ROOT/$traffic_name"
    mkdir -p "$TRAFFIC_ROOT"

    local_sctp_config=""
    local_rtp_config=""
    if [[ -f "$traffic_config_dir/sctp.csv" ]]; then
        local_sctp_config="$traffic_config_dir/sctp.csv"
        cp "$local_sctp_config" "$TRAFFIC_ROOT/sctp.csv"
    elif [[ -f "$traffic_config_dir/traffic_config.csv" ]]; then
        local_sctp_config="$traffic_config_dir/traffic_config.csv"
        cp "$local_sctp_config" "$TRAFFIC_ROOT/sctp.csv"
    fi

    if [[ -f "$traffic_config_dir/rtp.csv" ]]; then
        local_rtp_config="$traffic_config_dir/rtp.csv"
        cp "$local_rtp_config" "$TRAFFIC_ROOT/rtp.csv"
    fi

    if [[ -z "$local_sctp_config" ]]; then
        echo "Warning: no SCTP config found in $traffic_config_dir (expected sctp.csv)."
    fi
    if [[ -z "$local_rtp_config" ]]; then
        echo "Info: no RTP config found in $traffic_config_dir; defaults will be used."
    fi

    TRACE_COUNT=0

    while IFS= read -r -d '' trace_file; do
        trace_file_name=$(basename "$trace_file")
        trace_name="${trace_file_name%.pitree-trace}"

        # Create directories for this specific trace under traffic config
        EXPERIMENT_ROOT="$TRAFFIC_ROOT/$trace_name"
        PROFILES_DIR="$EXPERIMENT_ROOT/profiles"
        LOG_ROOT="$EXPERIMENT_ROOT/webrtc_logs"
        EMULATOR_LOG_DIR="$EXPERIMENT_ROOT/emulator_logs"
        STDOUT_DIR="$EXPERIMENT_ROOT/stdout"

        # Retry logic: up to 3 attempts
        attempt=1
        max_attempts=3
        success=false

        while [[ $attempt -le $max_attempts ]]; do
            echo -e "\n>>> Attempt $attempt of $max_attempts for trace: $trace_name"

            # Clean up and recreate directories for retry attempts
            if [[ $attempt -gt 1 ]]; then
                echo "  Cleaning up from previous failed attempt..."
                rm -rf "$EXPERIMENT_ROOT"
            fi

            mkdir -p "$PROFILES_DIR" "$LOG_ROOT" "$EMULATOR_LOG_DIR" "$STDOUT_DIR"

            if run_single_trace "$trace_file" "$local_sctp_config" "$local_rtp_config" "$traffic_name" "$attempt"; then
                echo "✓ Trace $trace_name succeeded on attempt $attempt"
                success=true
                TRACE_COUNT=$((TRACE_COUNT + 1))
                TOTAL_TRACE_COUNT=$((TOTAL_TRACE_COUNT + 1))
                break
            else
                echo "✗ Trace $trace_name failed on attempt $attempt"
                if [[ $attempt -lt $max_attempts ]]; then
                    echo "  Will retry after 5 seconds..."
                    sleep 5
                fi
                attempt=$((attempt + 1))
            fi
        done

        if [[ "$success" == "false" ]]; then
            echo -e "\n!!! ERROR: Trace $trace_name failed after $max_attempts attempts !!!"
            echo "!!! Cleaning up failed experiment results !!!"
            echo "  Removing directory: $EXPERIMENT_ROOT"
            rm -rf "$EXPERIMENT_ROOT"
            echo "!!! Stopping all experiments !!!"
            exit 1
        fi
    done < <(find "$TRACES_DIR" -maxdepth 1 -type f -name '*.pitree-trace' -print0 | sort -z)

    echo -e "\nCompleted $TRACE_COUNT traces for traffic config set: $traffic_name"
    echo "Results located at: $TRAFFIC_ROOT"
done

echo -e "\n========================================="
echo "ALL EXPERIMENTS COMPLETED"
echo "Total traces run: $TOTAL_TRACE_COUNT"
echo "Total traffic config sets: $TOTAL_TRAFFIC_CONFIGS"
echo "Results directory: $MAIN_EXPERIMENT_ROOT"
echo "=========================================\n"
