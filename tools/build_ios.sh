#!/bin/sh
# Build Jungle.app for the iOS simulator (default) or a device.
#
#   tools/build_ios.sh [simulator|device] [DISC_DIR]
#
# Needs Xcode and SDL2's source unpacked at build/deps/SDL2-2.30.9-src; SDL2 is
# built from its own Xcode project as a static library. DISC_DIR (default
# orig/cd/JUNGLE) is copied into the bundle as JUNGLE/, which is where the game
# looks first. A device build is signed with $IOS_SIGN_ID (an identity from your
# own Apple developer account); the simulator build is signed ad hoc.
set -eu
KIND=${1:-simulator}
DISC=${2:-orig/cd/JUNGLE}
SDL=build/deps/SDL2-2.30.9-src
OUT=build/ios-$KIND
case $KIND in
  simulator) SDK=iphonesimulator; TARGET=arm64-apple-ios13.0-simulator ;;
  device)    SDK=iphoneos;        TARGET=arm64-apple-ios13.0 ;;
  *) echo "usage: $0 [simulator|device] [DISC_DIR]" >&2; exit 2 ;;
esac
SYSROOT=$(xcrun --sdk $SDK --show-sdk-path)
LIB=build/deps/sdl2-ios-build/Release-$SDK/libSDL2.a
if [ ! -f "$LIB" ]; then
  xcodebuild -project $SDL/Xcode/SDL/SDL.xcodeproj -target "Static Library-iOS" -configuration Release \
    -sdk $SDK -arch arm64 ONLY_ACTIVE_ARCH=NO SYMROOT="$PWD/build/deps/sdl2-ios-build" >/dev/null
fi
APP=$OUT/Jungle.app
rm -rf "$APP"; mkdir -p "$APP"
xcrun --sdk $SDK clang -target $TARGET -isysroot "$SYSROOT" -O2 -Wall -Wno-unused-parameter \
  -I$SDL/include -Isrc -o "$APP/JungleGames" \
  src/jungle.c src/engine.c src/synth.c src/hires.c src/blit.c src/gif.c src/timing.c $SDL/src/main/uikit/SDL_uikit_main.c \
  "$LIB" -lm -liconv \
  -framework UIKit -framework Foundation -framework CoreGraphics -framework QuartzCore \
  -framework OpenGLES -framework Metal -framework AVFoundation -framework AudioToolbox \
  -framework CoreAudio -framework CoreMotion -framework GameController -framework CoreHaptics \
  -framework CoreBluetooth -framework CoreFoundation
cat > "$APP/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleExecutable</key><string>JungleGames</string>
  <key>CFBundleIdentifier</key><string>com.example.junglegames</string>
  <key>CFBundleName</key><string>Jungle Games</string>
  <key>CFBundleDisplayName</key><string>Jungle Games</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>MinimumOSVersion</key><string>13.0</string>
  <key>UIRequiresFullScreen</key><true/>
  <key>UIStatusBarHidden</key><true/>
  <key>UISupportedInterfaceOrientations</key>
  <array><string>UIInterfaceOrientationLandscapeLeft</string><string>UIInterfaceOrientationLandscapeRight</string></array>
  <key>UIDeviceFamily</key><array><integer>1</integer><integer>2</integer></array>
  <key>UILaunchStoryboardName</key><string></string>
</dict></plist>
PLIST
if [ -d "$DISC" ]; then
  mkdir -p "$APP/JUNGLE"
  cp "$DISC"/*.BIN "$DISC"/HYENA.TTF "$DISC"/JUNGA01.DLL "$DISC"/JUNGU01.DLL "$APP/JUNGLE/"
fi
if [ "$KIND" = device ]; then codesign -f -s "${IOS_SIGN_ID:?set IOS_SIGN_ID}" "$APP"; else codesign -f -s - "$APP"; fi
echo "$APP"
