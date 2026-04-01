#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$(cd "${ROOT_DIR}/.." && pwd)/build_darwin}"
BLENDER_BIN="${BLENDER_BIN:-${BUILD_DIR}/bin/Blender.app/Contents/MacOS/Blender}"

if [[ ! -x "${BLENDER_BIN}" ]]; then
  echo "Blender binary not found: ${BLENDER_BIN}" >&2
  echo "Set BUILD_DIR or BLENDER_BIN if needed." >&2
  exit 1
fi

if [[ $# -eq 0 ]]; then
  exec "${BLENDER_BIN}" --factory-startup
fi

exec "${BLENDER_BIN}" "$@"
