# iOS / iPadOS app: an Xcode project for signing and sideloading (see tools/build_ipa.sh).
#   cmake -S ios -B build/ios-xcode -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DTEAM=<team id>
cmake_minimum_required(VERSION 3.20)
project(JungleGames C)
set(CMAKE_OSX_DEPLOYMENT_TARGET 15.0)
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(SDL ${ROOT}/build/deps/SDL2-2.30.9-src)
set(DISC ${ROOT}/orig/cd/JUNGLE CACHE PATH "the disc's JUNGLE directory, bundled into the app")
set(BUNDLE_ID "com.mussyg.junglegames" CACHE STRING "")

file(GLOB DISC_FILES ${DISC}/*.BIN ${DISC}/HYENA.TTF ${DISC}/JUNGA01.DLL ${DISC}/JUNGU01.DLL)
set_source_files_properties(${DISC_FILES} PROPERTIES MACOSX_PACKAGE_LOCATION JUNGLE)

add_executable(JungleGames MACOSX_BUNDLE
  ${ROOT}/src/jungle.c ${ROOT}/src/engine.c ${ROOT}/src/synth.c ${ROOT}/src/hires.c
  ${ROOT}/src/blit.c ${ROOT}/src/gif.c ${ROOT}/src/timing.c ${ROOT}/src/pad.c ${ROOT}/src/iso.c ${SDL}/src/main/uikit/SDL_uikit_main.c
  ${DISC_FILES} ${CMAKE_CURRENT_SOURCE_DIR}/LaunchScreen.storyboard)
target_include_directories(JungleGames PRIVATE ${ROOT}/src ${SDL}/include)
target_compile_options(JungleGames PRIVATE -O2 -Wno-unused-parameter)
target_link_libraries(JungleGames PRIVATE
  ${ROOT}/build/deps/sdl2-ios-build/Release-$(PLATFORM_NAME)/libSDL2.a m iconv
  "-framework UIKit" "-framework Foundation" "-framework CoreGraphics" "-framework QuartzCore"
  "-framework OpenGLES" "-framework Metal" "-framework AVFoundation" "-framework AudioToolbox"
  "-framework CoreAudio" "-framework CoreMotion" "-framework GameController" "-framework CoreHaptics"
  "-framework CoreBluetooth" "-framework CoreFoundation")
set_target_properties(JungleGames PROPERTIES
  MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in
  MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
  XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID}
  XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${TEAM}"
  XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic
  XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Apple Development"
  XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
  XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET 15.0
  XCODE_ATTRIBUTE_ENABLE_BITCODE NO
  XCODE_ATTRIBUTE_SKIP_INSTALL NO
  XCODE_ATTRIBUTE_INSTALL_PATH "$(LOCAL_APPS_DIR)"
  )
