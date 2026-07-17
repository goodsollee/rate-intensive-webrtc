#!/usr/bin/env bash
#
# Build the libwebsockets dependency used by the peerconnection_client example
# for WebSocket signalling.
#
# libwebsockets is fetched as a gclient dependency (see DEPS) into
# third_party/libwebsockets. This script compiles it into
# third_party/libwebsockets/build/{lib,include}, which is where
# examples/BUILD.gn expects the headers and library to live.
#
# It is invoked automatically by the 'build_libwebsockets' gclient hook, but can
# also be run manually:  bash tools_webrtc/build_libwebsockets.sh
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
LWS_DIR="$SRC_ROOT/third_party/libwebsockets"
BUILD_DIR="$LWS_DIR/build"

if [[ ! -f "$LWS_DIR/CMakeLists.txt" ]]; then
  echo "[build_libwebsockets] $LWS_DIR not found or incomplete."
  echo "[build_libwebsockets] Run 'gclient sync' first to fetch libwebsockets. Skipping."
  exit 0
fi

# Nothing to do if the library is already built.
if [[ -f "$BUILD_DIR/lib/libwebsockets.a" || -f "$BUILD_DIR/lib/libwebsockets.so" ]]; then
  echo "[build_libwebsockets] Already built at $BUILD_DIR. Skipping."
  exit 0
fi

if ! command -v cmake >/dev/null 2>&1; then
  echo "[build_libwebsockets] cmake not found. Install it (e.g. 'sudo apt install cmake "
  echo "                      libssl-dev libcurl4-openssl-dev') and re-run this script."
  exit 0
fi

echo "[build_libwebsockets] Configuring libwebsockets (Release) ..."
cmake -S "$LWS_DIR" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DLWS_WITH_SSL=ON \
  -DLWS_WITH_LIBUV=OFF \
  -DLWS_WITHOUT_TESTAPPS=ON \
  -DLWS_WITHOUT_TEST_SERVER=ON \
  -DLWS_WITHOUT_TEST_CLIENT=ON

echo "[build_libwebsockets] Building ..."
cmake --build "$BUILD_DIR" -j"$(nproc 2>/dev/null || echo 4)"

echo "[build_libwebsockets] Done. Library is at $BUILD_DIR/lib."
