#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$(cd "${ROOT_DIR}/.." && pwd)/build_darwin}"

if [[ ! -d "${BUILD_DIR}" ]]; then
  echo "Build directory not found: ${BUILD_DIR}" >&2
  echo "Set BUILD_DIR=/path/to/build_dir if needed." >&2
  exit 1
fi

exec ninja -C "${BUILD_DIR}" install "$@"
