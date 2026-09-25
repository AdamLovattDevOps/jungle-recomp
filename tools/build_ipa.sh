#!/bin/sh
# Build a signed Jungle Games .ipa for sideloading onto your own iPhone/iPad.
#   tools/build_ipa.sh [TEAM_ID]        -> build/ios-xcode/ipa/Jungle Games.ipa
# Needs Xcode signed in to an Apple ID (Settings > Accounts) whose team has the
# device registered, SDL2's source at build/deps/SDL2-2.30.9-src, and the disc
# at orig/cd/JUNGLE (bundled into the app). Signing is automatic.
set -eu
cd "$(dirname "$0")/.."
TEAM=${1:-${IOS_TEAM:?pass your Apple team id (Xcode > Settings > Accounts)}}
SDL=build/deps/SDL2-2.30.9-src
[ -f build/deps/sdl2-ios-build/Release-iphoneos/libSDL2.a ] || \
  xcodebuild -project $SDL/Xcode/SDL/SDL.xcodeproj -target "Static Library-iOS" -configuration Release \
    -sdk iphoneos -arch arm64 ONLY_ACTIVE_ARCH=NO SYMROOT="$PWD/build/deps/sdl2-ios-build" >/dev/null
cmake -S ios -B build/ios-xcode -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DTEAM="$TEAM" >/dev/null
rm -rf build/ios-xcode/JungleGames.xcarchive build/ios-xcode/ipa
xcodebuild -project build/ios-xcode/JungleGames.xcodeproj -scheme JungleGames -configuration Release \
  -sdk iphoneos -destination generic/platform=iOS -allowProvisioningUpdates \
  -archivePath build/ios-xcode/JungleGames.xcarchive archive -quiet
cat > build/ios-xcode/export.plist <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>method</key><string>development</string>
  <key>teamID</key><string>$TEAM</string>
  <key>signingStyle</key><string>automatic</string>
  <key>compileBitcode</key><false/>
</dict></plist>
PLIST
xcodebuild -exportArchive -archivePath build/ios-xcode/JungleGames.xcarchive -exportPath build/ios-xcode/ipa \
  -exportOptionsPlist build/ios-xcode/export.plist -allowProvisioningUpdates -quiet
ls -la "build/ios-xcode/ipa/Jungle Games.ipa"
