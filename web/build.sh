#!/bin/sh
# Build the WebAssembly version - both a multi-threaded and a single-threaded
# variant - and assemble them into build-web/ next to the loader.
#
# Requires the Emscripten SDK (emcc / emcmake on PATH).  The first build also
# fetches and compiles SDL3 to WebAssembly (twice: with and without pthreads),
# which takes a few minutes.
#
# The result, build-web/, is fully static and can be served from any host,
# GitHub Pages included.  index.html tries to cross-origin-isolate the page
# (via enable-threads.js) and loads the multi-threaded build when that
# succeeds, otherwise the single-threaded build.  Serve over HTTP, e.g.:
#   python3 -m http.server -d build-web
set -e
cd "$(dirname "$0")/.."

emcmake cmake -S . -B build-web-st -DCMAKE_BUILD_TYPE=Release -DSHOGI_PTHREAD=OFF
cmake --build build-web-st -j

emcmake cmake -S . -B build-web-mt -DCMAKE_BUILD_TYPE=Release -DSHOGI_PTHREAD=ON
cmake --build build-web-mt -j

mkdir -p build-web
rm -rf build-web/*
cp build-web-st/shogi-st.js build-web-st/shogi-st.wasm build-web/
cp build-web-mt/shogi-mt.js build-web-mt/shogi-mt.wasm build-web/
cp web/index.html web/enable-threads.js build-web/

echo
echo "Built build-web/: index.html, enable-threads.js, shogi-{st,mt}.{js,wasm}"
