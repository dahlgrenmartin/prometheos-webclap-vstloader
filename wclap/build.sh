#!/usr/bin/env bash
# Builds the vstloader WebCLAP shim (build/vstloader.wasm) with wasi-sdk.
# Its memory is imported and shared (the threads target), so the host can hand
# it to the runtime page; the module itself never spawns a thread.
#   WASI_SDK  wasi-sdk directory (default /opt/wasi-sdk)
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
cd "$here"
WASI_SDK=${WASI_SDK:-/opt/wasi-sdk}
mkdir -p build
"$WASI_SDK/bin/clang" --target=wasm32-wasip1-threads -O2 -std=c11 -Wall -Wextra -Werror \
  -mexec-model=reactor -matomics -mbulk-memory \
  -Ivendor/clap/include -Iinclude -Iwclap \
  -Wl,--import-memory,--shared-memory,--max-memory=268435456 \
  -Wl,--export=malloc,--export=free,--export=clap_entry,--export-table,--growable-table \
  -o build/vstloader.wasm wclap/vstloader.c
echo "built build/vstloader.wasm ($(wc -c < build/vstloader.wasm) bytes)"
