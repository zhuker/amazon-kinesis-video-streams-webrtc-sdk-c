#!/usr/bin/env bash
# Build and run Puppeteer-based browser interop tests for DataChannels.
#
# Usage:
#   ./scripts/test-browser.sh              # full build + test
#   ./scripts/test-browser.sh --skip-build # reuse existing build
#   ./scripts/test-browser.sh --headed     # run with visible Chrome window

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SUPERBUILD_DIR="${ROOT_DIR}/superbuild"
BUILD_DIR="${SUPERBUILD_DIR}/build-browser"
BROWSER_TEST_DIR="${ROOT_DIR}/browser-test"

SKIP_BUILD=false
EXTRA_ARGS=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-build) SKIP_BUILD=true; shift ;;
        --headed) EXTRA_ARGS="$EXTRA_ARGS --headed"; shift ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# 1. Build dcTestServer via superbuild
if [[ "$SKIP_BUILD" == false ]]; then
    echo "=== Building dcTestServer ==="
    cmake -B "$BUILD_DIR" -S "$SUPERBUILD_DIR" \
        -DBUILD_TEST=OFF \
        -DBUILD_BROWSER_TEST=ON \
        -DENABLE_SIGNALING=OFF \
        -DENABLE_DATA_CHANNEL=ON \
        -DENABLE_NATIVE_SCTP=ON
    cmake --build "$BUILD_DIR" --target dcTestServer -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"
fi

DC_SERVER="${BUILD_DIR}/dcTestServer"
if [[ ! -x "$DC_SERVER" ]]; then
    echo "ERROR: dcTestServer not found at $DC_SERVER"
    exit 1
fi

# 2. Install Node.js deps (Puppeteer)
echo "=== Installing Puppeteer ==="
cd "$BROWSER_TEST_DIR"
npm install --no-fund --no-audit 2>&1 | tail -1

# 3. Run tests
echo "=== Running browser interop tests ==="
cd "$ROOT_DIR"
node "$BROWSER_TEST_DIR/runner.mjs" "$DC_SERVER" "$BROWSER_TEST_DIR" $EXTRA_ARGS
