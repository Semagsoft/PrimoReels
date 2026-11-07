#!/usr/bin/env bash
# Run the PrimoReels test suite with the dummy media backend so no audio
# is routed to the system speakers (sine-tone test clips stay silent).
#
# Usage:
#   ./runtests.sh [ctest args...]
#
# Defaults to running all tests with output on failure, mirroring CI.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${1:-build}"
shift || true

export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"
export QT_MEDIA_BACKEND="${QT_MEDIA_BACKEND:-dummy}"

exec ctest --test-dir "$BUILD_DIR" --output-on-failure "$@"
