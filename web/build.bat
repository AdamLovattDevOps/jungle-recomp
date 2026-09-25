@echo off
rem Build the WebAssembly version into web\dist on Windows (Emscripten from emsdk).
rem   web\build.bat [path\to\emsdk]
setlocal
set EMSDK_DIR=%~1
if "%EMSDK_DIR%"=="" set EMSDK_DIR=C:\srv\emsdk
call "%EMSDK_DIR%\emsdk_env.bat" >nul 2>&1
cd /d "%~dp0.."
if not exist web\dist mkdir web\dist
call emcc -O2 -Isrc src/jungle.c src/engine.c src/synth.c src/hires.c src/blit.c src/gif.c src/timing.c src/pad.c src/iso.c -sUSE_SDL=2 -sASYNCIFY -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=268435456 -sSTACK_SIZE=1048576 -sINVOKE_RUN=0 -sFORCE_FILESYSTEM=1 -lidbfs.js -sEXPORTED_RUNTIME_METHODS=callMain,FS,IDBFS,cwrap -sEXPORTED_FUNCTIONS=_main,_web_key -sENVIRONMENT=web -o web/dist/jungle.js || exit /b 1
copy /y web\index.html web\dist\ >nul
copy /y web\loader.js web\dist\ >nul
dir /b web\dist
