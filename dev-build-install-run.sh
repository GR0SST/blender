#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "==> Building Blender"
"${ROOT_DIR}/dev-build.sh"

echo "==> Installing Blender"
"${ROOT_DIR}/dev-install.sh"

echo "==> Running Blender"
"${ROOT_DIR}/dev-run.sh" "$@"
