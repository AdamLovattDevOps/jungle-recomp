#!/bin/sh
# Inside the build image (Containerfile): compile the engine against the image's SDL2
# and pack build/appimage/Jungle_Games-x86_64.AppImage. Run through tools/appimage/build.sh.
set -eu
cd /src
OUT=build/appimage
APPDIR=$OUT/AppDir
rm -rf "$APPDIR" && mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" "$APPDIR/usr/share/jungle-games"

cc -O2 -Wall -Wextra -Wno-unused-parameter $(sdl2-config --cflags) -o "$APPDIR/usr/bin/jungle" \
   src/jungle.c src/engine.c src/synth.c src/hires.c src/blit.c src/gif.c src/timing.c src/pad.c src/iso.c \
   $(sdl2-config --libs) -lm -Wl,-rpath,'$ORIGIN/../lib'
strip "$APPDIR/usr/bin/jungle"
cp -L /opt/sdl2/lib/libSDL2-2.0.so.0 "$APPDIR/usr/lib/"

cp tools/appimage/AppRun "$APPDIR/AppRun" && chmod +x "$APPDIR/AppRun"
cp tools/appimage/jungle-games.desktop "$APPDIR/"
python3 tools/appimage/icon.py "$APPDIR/jungle-games.png"
cp orig/CHECKSUMS.sha256 "$APPDIR/usr/share/jungle-games/"

# nothing else is bundled: glibc and the display/audio stacks are the host's
echo "== libraries the engine needs from the host:"
LD_LIBRARY_PATH="$APPDIR/usr/lib" ldd "$APPDIR/usr/bin/jungle" | grep -v "$APPDIR" | sed 's/ (0x.*//'

ARCH=x86_64 appimagetool --no-appstream --runtime-file /opt/runtime-x86_64 "$APPDIR" "$OUT/Jungle_Games-x86_64.AppImage"
ls -la "$OUT/Jungle_Games-x86_64.AppImage"
