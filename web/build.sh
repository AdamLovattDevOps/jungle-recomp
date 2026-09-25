#!/bin/sh
# Build the WebAssembly version into web/dist (needs Emscripten: emcc on PATH).
#   web/build.sh            then serve web/dist with any static file server.
# The page asks each player for their own disc; no game data is built in.
set -eu
cd "$(dirname "$0")/.."
mkdir -p web/dist
emcc -O2 -Isrc src/jungle.c src/engine.c src/synth.c src/hires.c src/blit.c src/gif.c src/timing.c src/pad.c src/iso.c \
  -sUSE_SDL=2 -sASYNCIFY -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=268435456 -sSTACK_SIZE=1048576 \
  -sINVOKE_RUN=0 -sFORCE_FILESYSTEM=1 -lidbfs.js \
  -sEXPORTED_RUNTIME_METHODS=callMain,FS,IDBFS,cwrap -sEXPORTED_FUNCTIONS=_main,_web_key -sENVIRONMENT=web \
  -o web/dist/jungle.js
cp web/index.html web/loader.js web/dist/
echo "web/dist: $(ls web/dist | tr '\n' ' ')"
