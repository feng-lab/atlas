#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_PATH="${SCRIPT_DIR}/macos_window_capture_sckit.swift"
OUTPUT_PATH="${1:-/private/tmp/macos_window_capture_sckit}"

swiftc -parse-as-library "${SOURCE_PATH}" \
  -o "${OUTPUT_PATH}" \
  -framework ScreenCaptureKit \
  -framework CoreMedia \
  -framework CoreVideo \
  -framework AppKit \
  -framework ApplicationServices

echo "${OUTPUT_PATH}"
