#!/bin/bash
set -e

CSV=""
SERVER="localhost"
PORT=8888
CLIENT="./peerconnection_client"

usage() {
  echo "Usage: $0 --csv <file> [--server <host>] [--port <port>]" >&2
  exit 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --csv)
      CSV="$2"; shift 2;;
    --server)
      SERVER="$2"; shift 2;;
    --port)
      PORT="$2"; shift 2;;
    *)
      usage;;
  esac
done

if [[ -z "$CSV" ]]; then
  usage
fi

$CLIENT --traffic_csv="$CSV" --server="$SERVER" --port="$PORT"
python3 "$(dirname "$0")/analyze_logs.py" rtp_log.csv sctp_log.csv
