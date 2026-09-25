#!/bin/sh
# Build the Linux AppImage in a container: podman, or docker.
#
#   tools/appimage/build.sh          -> build/appimage/Jungle_Games-x86_64.AppImage
#
# No game data goes into it. On first run it finds your disc (see AppRun).
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ENGINE="${CONTAINER_ENGINE:-$(command -v podman || command -v docker || true)}"
[ -n "$ENGINE" ] || { echo "needs podman or docker" >&2; exit 1; }
"$ENGINE" build -t jungle-appimage -f "$ROOT/tools/appimage/Containerfile" "$ROOT/tools/appimage"
USER_OPT=""
case "$ENGINE" in *docker*) USER_OPT="--user $(id -u):$(id -g)";; esac   # rootless podman already maps root to you
"$ENGINE" run --rm $USER_OPT -v "$ROOT":/src:Z jungle-appimage sh tools/appimage/package.sh
