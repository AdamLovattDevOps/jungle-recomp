# Building and running on each platform

The port is one C99 program (`src/jungle.c`, `src/engine.c`, `src/synth.c` and a few helpers)
on SDL2. It has no other dependencies. No game data ships with it: every build needs the
`JUNGLE` directory from your own copy of the disc (the `.BIN` files, `HYENA.TTF`, `JUNGA01.DLL`
and `JUNGU01.DLL`; the DLLs are read for their ADPCM and trigonometry tables, never executed).

Run with no arguments and the game looks for `JUNGLE/JUNGLE.BIN` next to the executable, then
for `orig/cd/JUNGLE/JUNGLE.BIN` under the current directory. Otherwise it takes a path:
`jungle path/to/JUNGLE/JUNGLE.BIN --game`.

Settings and high scores are written to `7THLEVEL.INI` in SDL's preferences folder for
"7th Level / Jungle Games":

| Platform | Location |
|---|---|
| macOS | `~/Library/Application Support/7th Level/Jungle Games/` |
| Windows | `%APPDATA%\7th Level\Jungle Games\` |
| Linux | `~/.local/share/7th Level/Jungle Games/` |
| PS Vita | `ux0:data/7th Level/Jungle Games/` |

## macOS

```sh
brew install sdl2
make
./jungle
```

## Linux

```sh
sudo apt install build-essential libsdl2-dev     # or your distribution's equivalent
make
./jungle
```

`cmake -B build/cmake && cmake --build build/cmake` also works.

## Windows

Cross-compiled from macOS or Linux with MinGW-w64. Unpack SDL2's `SDL2-devel-2.x-mingw`
package into `build/deps`, then:

```sh
make windows            # build/win64/jungle.exe and SDL2.dll
```

Copy the disc's `JUNGLE` directory next to `jungle.exe`. With Visual Studio, configure
`CMakeLists.txt` with `SDL2_DIR` pointing at SDL2's VC development package.

## PS Vita

Needs [VitaSDK](https://vitasdk.org) and its SDL2 package:

```sh
git clone https://github.com/vitasdk/vdpm && cd vdpm
./bootstrap-vitasdk.sh --install-dir ~/vitasdk
export VITASDK=~/vitasdk PATH=~/vitasdk/bin:$PATH
vdpm install sdl2
cd jungle-recomp
cmake -B build/vita -DVITA=ON -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
cmake --build build/vita  # build/vita/jungle.vpk, title ID JUNG00001
```

Install the VPK with VitaShell and copy the disc's `JUNGLE` directory contents to
`ux0:data/jungle/`. The 800×600 canvas is letterboxed to 960×544. The front touch screen is the
mouse, the d-pad the arrows, Cross/Circle the games' X/Z keys, L/R Pinball's flippers,
Start Enter and Select Escape.

The game uses about 40 MB at most, well inside the Vita's memory.

## iOS

Needs Xcode and SDL2's source unpacked at `build/deps/SDL2-2.30.9-src`:

```sh
tools/build_ios.sh simulator          # build/ios-simulator/Jungle.app
xcrun simctl install booted build/ios-simulator/Jungle.app
xcrun simctl launch booted com.example.junglegames
IOS_SIGN_ID="Apple Development: ..." tools/build_ios.sh device   # signed with your own account
```

The script builds SDL2 from its Xcode project as a static library, and copies the disc's
`JUNGLE` directory (default `orig/cd/JUNGLE`) into the bundle, where the game looks first. The
app runs full screen in landscape. Touches arrive as mouse events.

## Controllers

Any SDL game controller works on every platform; the mapping per game is in
[CONTROLLERS.md](CONTROLLERS.md).

## What has been tested

| Platform | State |
|---|---|
| macOS (arm64) | built and played; the engine harness checks every game headless |
| Linux | built and played on Bazzite (Legion Go 2) as the AppImage, launched from Steam |
| Windows | `jungle.exe` cross-compiles cleanly; not yet run |
| PS Vita | `jungle.vpk` builds with VitaSDK 2026.08; not yet run on hardware or in Vita3K |
| High-resolution art | macOS: every scene exported and upscaled; the renderer matches the classic frame exactly without the cache (`make e2e`) |
| iOS | runs in the iOS 18.1 simulator (iPad Air 11-inch): boots through the intro; touch input not yet exercised |

## High-resolution art

`make hires` builds a 3x copy of every bitmap on your disc with Real-ESRGAN's anime model, a
neural upscaler that redraws the dithered 256-colour art as clean painted shapes. It needs
[realesrgan-ncnn-vulkan](https://github.com/xinntao/Real-ESRGAN/releases) (BSD-3, runs on the
GPU through Vulkan; MoltenVK on macOS) unpacked at `build/deps/esrgan`, and Pillow. On an M3 Pro
the whole disc takes about 15 minutes and 1 GB. Nothing it writes is committed.

```sh
curl -LO https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-macos.zip
unzip realesrgan-ncnn-vulkan-20220424-macos.zip -d build/deps/esrgan
python3 tools/upscale.py --scale 4   # build/hires/x4: the model's own 4x output (1.4 GB)
python3 tools/upscale.py --scale 3   # build/hires/x3: resampled 3x (0.8 GB), optional
./jungle                             # F9 cycles: original, upscaled 1x, 2x, 3x, 4x
```

Art levels: the original pixels, and the upscaled art at 1x, 2x, 3x and 4x. 1x and 2x are the x4
art box-filtered down as it loads (1x is the cleaned-up art at the original resolution), 3x reads
x3/, 4x reads x4/. F9 cycles through the levels present and the window title names the current
one; `JUNGLE_ART=original|1|2|3|4` picks the starting level. Higher levels (6x, 8x) are a second
upscale pass over x4 and need about 9 GB more disk.

The game looks for the cache in `$JUNGLE_HIRES`, then `hires/` beside the executable, then
`build/hires`. With a cache it opens a 1200x900 window (2400x1800 pixels on Retina) and draws
every bitmap as a GPU texture at the window's full resolution; bitmaps missing from the cache
are drawn from their original pixels, so a partial cache still works. `JUNGLE_CLASSIC=1`
disables it. The game logic is untouched: hit tests, collisions and scripts all still see the
800x600 canvas.

## Music

The disc's music is General MIDI (type 4 resources; see `notes/type4-midi-format.md`), which
Windows sent to whatever synthesiser the sound card had. `src/synth.c` stands in for that
device: one subtractive patch per GM instrument family plus synthesised drums, mixed at
22,050 Hz with the sound effects.
