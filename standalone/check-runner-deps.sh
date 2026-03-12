#!/bin/bash
# Check self-hosted runner readiness for moxygen CI/publish jobs.
# Run on the target machine: bash standalone/check-runner-deps.sh

set -e

echo "=== OS ==="
cat /etc/os-release 2>/dev/null | head -3 || echo "Unknown OS"

echo ""
echo "=== CPU / RAM / Disk ==="
echo "  CPUs: $(nproc)"
echo "  RAM:  $(free -h | awk '/Mem:/ {print $2}')"
df -h / | awk 'NR==2 {printf "  Disk: %s used of %s (%s free)\n", $3, $2, $4}'

echo ""
echo "=== Required tools ==="
for cmd in git cmake ninja gcc g++ make ccache curl sudo; do
    if command -v $cmd &>/dev/null; then
        ver=$($cmd --version 2>&1 | head -1)
        echo "  ✓ $cmd  ($ver)"
    else
        echo "  ✗ $cmd  MISSING"
    fi
done

echo ""
echo "=== Docker (needed for container jobs) ==="
if command -v docker &>/dev/null; then
    echo "  ✓ docker installed"
    if docker info --format '{{.ServerVersion}}' &>/dev/null; then
        echo "  ✓ docker daemon running ($(docker info --format '{{.ServerVersion}}' 2>/dev/null))"
    else
        echo "  ✗ docker daemon not running"
    fi
    if docker ps &>/dev/null; then
        echo "  ✓ current user can run docker"
    else
        echo "  ✗ current user cannot run docker (add to docker group)"
    fi
else
    echo "  ✗ docker not installed"
    echo "    needed for: container: debian:bookworm jobs"
    echo "    install: https://docs.docker.com/engine/install/ubuntu/"
fi

echo ""
echo "=== Build libraries (Ubuntu/Debian apt) ==="
if ! command -v dpkg &>/dev/null; then
    echo "  (skipped — not a Debian-based system)"
else
    for pkg in build-essential ninja-build gperf \
               libssl-dev libunwind-dev libgoogle-glog-dev libgflags-dev \
               libdouble-conversion-dev libevent-dev libsodium-dev libzstd-dev \
               libboost-all-dev libfmt-dev zlib1g-dev libc-ares-dev; do
        if dpkg -s "$pkg" &>/dev/null 2>&1; then
            echo "  ✓ $pkg"
        else
            echo "  ✗ $pkg  MISSING"
        fi
    done
fi

echo ""
echo "=== GitHub Actions runner ==="
for d in ~/actions-runner /opt/actions-runner /home/runner/actions-runner; do
    if [[ -d "$d" ]]; then
        echo "  ✓ Runner dir: $d"
        if [[ -x "$d/run.sh" ]]; then
            "$d/config.sh" --version 2>/dev/null || true
        fi
    fi
done

echo ""
echo "=== Summary ==="
echo "To install missing system deps: bash standalone/install-system-deps.sh"
echo "To install docker: https://docs.docker.com/engine/install/ubuntu/"
echo "To install ccache: sudo apt-get install -y ccache"
