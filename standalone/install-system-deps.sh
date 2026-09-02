#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
# This source code is licensed under the Apache 2.0 license found in the
# LICENSE file in the root directory of this source tree.

# Installs system dependencies required for standalone moxygen build

set -e

# apt can hang past its own transfer timeouts: a dribbling mirror defeats the
# 30s read timeout, and a wedged https helper never fires it (the helper is
# what enforces it). Progress watchdog: healthy apt prints continuously, so
# kill the call after 2 minutes of output silence; hard-cap the whole call at
# 15 minutes as a backstop against a mirror that trickles forever.
apt_get() {
    local log pid rc size prev=-1 idle=0
    log=$(mktemp)
    sudo timeout -k 30 900 apt-get -o Acquire::Retries=3 -o Acquire::http::Timeout=30 -o Acquire::https::Timeout=30 "$@" >"$log" 2>&1 &
    pid=$!
    tail -f --pid="$pid" "$log" &
    while kill -0 "$pid" 2>/dev/null; do
        sleep 5
        size=$(stat -c %s "$log" 2>/dev/null || echo -1)
        if [ "$size" = "$prev" ]; then
            idle=$((idle + 5))
            if [ "$idle" -ge 120 ]; then
                echo "ERROR: apt-get made no progress for ${idle}s; aborting" >&2
                sudo kill "$pid" 2>/dev/null || true
                sleep 5
                sudo kill -9 "$pid" 2>/dev/null || true
                wait "$pid" 2>/dev/null || true
                rm -f "$log"
                return 124
            fi
        else
            prev=$size; idle=0
        fi
    done
    wait "$pid" && rc=0 || rc=$?
    rm -f "$log"
    return "$rc"
}

install_ubuntu() {
    echo "Installing dependencies for Ubuntu/Debian..."
    apt_get update
    apt_get install -y \
        build-essential \
        cmake \
        ninja-build \
        git \
        pkg-config \
        libssl-dev \
        libunwind-dev \
        libgoogle-glog-dev \
        libgflags-dev \
        libdouble-conversion-dev \
        libevent-dev \
        libsodium-dev \
        libzstd-dev \
        libboost-dev \
        libboost-context-dev \
        libboost-filesystem-dev \
        libboost-program-options-dev \
        libboost-regex-dev \
        libboost-thread-dev \
        libfmt-dev \
        zlib1g-dev \
        libc-ares-dev \
        python3 \
        gperf
}

install_fedora() {
    echo "Installing dependencies for Fedora/CentOS/RHEL..."
    sudo dnf install -y \
        cmake \
        git \
        openssl-devel \
        glog-devel \
        gflags-devel \
        double-conversion-devel \
        libevent-devel \
        libsodium-devel \
        libzstd-devel \
        boost-devel \
        fmt-devel \
        zlib-devel \
        c-ares-devel \
        python3 \
        gperf
    # ninja-build is available on Fedora but not CentOS/RHEL base repos.
    # Try to install it; if unavailable, warn with alternatives.
    if ! command -v ninja &>/dev/null; then
        if ! sudo dnf install -y ninja-build 2>/dev/null; then
            echo ""
            echo "WARNING: ninja-build not available in configured repos."
            echo "Install ninja manually, e.g.:"
            echo "  pip install ninja"
            echo "  # or download from https://github.com/ninja-build/ninja/releases"
        fi
    fi
}

install_macos() {
    echo "Installing dependencies for macOS..."
    brew install \
        cmake \
        ninja \
        openssl@3 \
        glog \
        gflags \
        double-conversion \
        libevent \
        libsodium \
        zstd \
        boost \
        fmt \
        c-ares \
        gperf \
        brotli
}

# Detect OS - check macOS first
if [[ "$(uname)" == "Darwin" ]]; then
    install_macos
elif [[ -f /etc/os-release ]]; then
    . /etc/os-release
    case "$ID" in
        ubuntu|debian)
            install_ubuntu
            ;;
        fedora|centos|rhel)
            install_fedora
            ;;
        *)
            # Derivatives (Linux Mint, Pop!_OS, LMDE, Rocky, Alma, ...)
            # report their own ID; dispatch on ID_LIKE instead.
            case " ${ID_LIKE:-} " in
                *" ubuntu "*|*" debian "*)
                    install_ubuntu
                    ;;
                *" fedora "*|*" rhel "*|*" centos "*)
                    install_fedora
                    ;;
                *)
                    echo "Unsupported Linux distribution: $ID (ID_LIKE='${ID_LIKE:-}')"
                    echo "Please install dependencies manually (see README.md)"
                    exit 1
                    ;;
            esac
            ;;
    esac
else
    echo "Unsupported operating system"
    exit 1
fi

echo ""
echo "System dependencies installed successfully!"
echo "You can now build moxygen with:"
echo "  cmake -B _build -S standalone -G Ninja"
echo "  cmake --build _build"
