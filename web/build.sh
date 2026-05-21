#!/bin/sh
# Build the single-threaded WebAssembly version.
#
# Requires the Emscripten SDK (emcc / emcmake on PATH). The first build also
# fetches and compiles SDL3 to WebAssembly, which takes a few minutes.
#
# Output: build-web/shogi.{html,js,wasm} - a single-threaded build that needs
# no SharedArrayBuffer and no COOP/COEP headers, so it can be served from any
# static host, GitHub Pages included. Serve over HTTP (not file://), e.g.:
#   python3 -m http.server -d build-web
set -e
cd "$(dirname "$0")/.."
emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release
cmake --build build-web -j
echo
echo "Built: build-web/shogi.html (+ shogi.js, shogi.wasm)"
