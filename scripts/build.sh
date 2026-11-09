#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
if ! command -v em++ >/dev/null; then
  sdk="${EMSDK:-${XDG_CACHE_HOME:-$HOME/.cache}/colonizing-pig-emsdk}"
  if [[ ! -f "$sdk/emsdk" ]]; then
    git clone --depth 1 https://github.com/emscripten-core/emsdk.git "$sdk"
  fi
  "$sdk/emsdk" install 7.0.0
  "$sdk/emsdk" activate 7.0.0
  source "$sdk/emsdk_env.sh"
fi
make build
