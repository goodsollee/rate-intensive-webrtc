FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive TZ=Etc/UTC

# Build deps (WebRTC + X11 + audio + LWS)
RUN apt-get update && apt-get install -y \
    git curl ca-certificates pkg-config \
    python3 python3-venv python3-pip \
    build-essential cmake ninja-build \
    clang lld \
    libgtk-3-dev libglib2.0-dev \
    libnss3-dev libasound2-dev libpulse-dev \
    libx11-dev libxext-dev libxcomposite-dev libxrandr-dev \
    libxi-dev libxrender-dev libxtst-dev libxdamage-dev libxfixes-dev \
    libdrm-dev libgbm-dev libxkbcommon-dev libxshmfence-dev \
    libpci-dev libudev-dev libcurl4-openssl-dev \
    libwebsockets-dev zlib1g-dev libssl-dev \
    pkg-config \
 && rm -rf /var/lib/apt/lists/*

# depot_tools
RUN git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git /opt/depot_tools
ENV PATH="/opt/depot_tools:${PATH}"

# **Bootstrap depot_tools** (fix for python3_bin_reldir.txt not found)
# If you prefer no auto-update: add `ENV DEPOT_TOOLS_UPDATE=0` and keep `gclient --version`.
RUN /opt/depot_tools/update_depot_tools && gclient --version

# Project
RUN git clone https://github.com/goodsollee/webrtc-testbed-platform.git /src
WORKDIR /src

# Build
ENV CC=clang CXX=clang++
RUN gn gen out/Default --args='is_debug=true is_component_build=false rtc_build_examples=true rtc_use_x11=true treat_warnings_as_errors=false use_sysroot=false' \
 && ninja -C out/Default peerconnection_client peerconnection_server

CMD ["./out/Default/peerconnection_client"]
