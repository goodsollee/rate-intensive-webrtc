#!/usr/bin/env bash
set -euo pipefail

################################################################################
# Usage & defaults
################################################################################
usage() {
    echo "Usage: $0 <room_id> <is_sender> [--min N] [--max N] [--y4m PATH] [--sctp PATH]"
    echo "  room_id   : Room identifier"
    echo "  is_sender : true | false"
    echo "  --min N   : (optional) playout-delay min in ms   [default: 0]"
    echo "  --max N   : (optional) playout-delay max in ms   [default: 10]"
    echo "  --y4m PATH: (optional) path to *.y4m file to send"
    echo "  --sctp PATH: (optional) path to sctp_traffic CSV (sent as --sctp_csv=PATH)"
    exit 1
}

# hard defaults
min_delay=0
max_delay=10
y4m_path=""
sctp_csv=""

################################################################################
# Positional args (room_id, is_sender) + option parsing
################################################################################
positional=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --min)   min_delay="$2"; shift 2 ;;
        --max)   max_delay="$2"; shift 2 ;;
        --y4m)   y4m_path="$2"; shift 2 ;;
        --sctp)  sctp_csv="$2"; shift 2 ;;
        -h|--help) usage ;;
        *) positional+=("$1"); shift ;;
    esac
done
set -- "${positional[@]}"

[[ ${#positional[@]} -lt 2 ]] && usage
room_id="$1"
is_sender="$2"

################################################################################
# Simple validation
################################################################################
[[ "$is_sender" != "true" && "$is_sender" != "false" ]] && {
    echo "Error: is_sender must be 'true' or 'false'"; exit 1; }

[[ "$min_delay" =~ ^[0-9]+$ && "$max_delay" =~ ^[0-9]+$ ]] || {
    echo "Error: --min and --max must be integers"; exit 1; }

(( min_delay <= max_delay )) || {
    echo "Error: --min must be ≤ --max"; exit 1; }

[[ -n "$y4m_path" && ! -f "$y4m_path" ]] && {
    echo "Error: y4m file does not exist: $y4m_path"; exit 1; }

[[ -n "$sctp_csv" && ! -f "$sctp_csv" ]] && {
    echo "Error: sctp CSV file does not exist: $sctp_csv"; exit 1; }

# Optional light sanity on extensions (non-fatal warnings)
if [[ -n "$y4m_path" && ! "$y4m_path" =~ \.y4m$ ]]; then
    echo "Warning: --y4m provided but file does not end with .y4m: $y4m_path"
fi
if [[ -n "$sctp_csv" && ! "$sctp_csv" =~ \.csv$ ]]; then
    echo "Warning: --sctp provided but file does not end with .csv: $sctp_csv"
fi

################################################################################
# Build and run peerconnection_client
################################################################################
headless=$([[ "$is_sender" == "true" ]] && echo "true" || echo "false")

# Start the bundled signalling server first, in another shell:
#   python3 signaling_server.py --host 0.0.0.0 --port 8888
# The sender (initiator) uses --autocall so it offers as soon as the peer joins.
autocall=$([[ "$is_sender" == "true" ]] && echo "true" || echo "false")

# headless still calls gtk_init, which needs an X display; use a virtual one.
xvfb=""
if [[ -z "${DISPLAY:-}" ]] && command -v xvfb-run >/dev/null 2>&1; then
    xvfb="xvfb-run -a"
fi

cmd="${xvfb} ../out/Default/peerconnection_client \
      --server=localhost --port=8888 --server_scheme=http \
      --autoconnect=true --autocall=${autocall} \
      --is_sender=${is_sender} \
      --room_id=${room_id} \
      --headless=${headless} \
      --force_fieldtrials=\"WebRTC-ForcePlayoutDelay/min_ms:${min_delay},max_ms:${max_delay}/\""

[[ -n "$y4m_path" ]] && cmd+=" --y4m_path=${y4m_path}"
[[ -n "$sctp_csv" ]] && cmd+=" --sctp_csv=${sctp_csv}"

#pulseaudio --start
eval "${cmd}"
