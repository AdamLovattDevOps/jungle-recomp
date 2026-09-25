# jungle-recomp — portable build (Linux, Windows, macOS, iOS, PS Vita; see docs/PLATFORMS.md)
cmake_minimum_required(VERSION 3.16)

if(DEFINED ENV{VITASDK} AND VITA)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "")
endif()

project(jungle C)
set(CMAKE_C_STANDARD 99)

find_package(SDL2 REQUIRED)

add_executable(jungle
    src/jungle.c src/engine.c src/synth.c src/hires.c src/blit.c src/gif.c src/timing.c src/pad.c src/iso.c)
target_include_directories(jungle PRIVATE src)
if(TARGET SDL2::SDL2main)
    target_link_libraries(jungle PRIVATE SDL2::SDL2main)
endif()
if(TARGET SDL2::SDL2-static AND VITA)
    target_link_libraries(jungle PRIVATE SDL2::SDL2-static)
else()
    target_link_libraries(jungle PRIVATE SDL2::SDL2)
endif()
if(NOT MSVC)
    target_link_libraries(jungle PRIVATE m)
    target_compile_options(jungle PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
endif()

# PS Vita: cmake -DVITA=ON (with VITASDK set) builds jungle.vpk.
# The game data goes in ux0:data/jungle/ (the JUNGLE folder's contents).
if(VITA)
    include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
    set(VITA_APP_NAME "Jungle Games")
    set(VITA_TITLEID  "JUNG00001")
    set(VITA_VERSION  "01.00")
    set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")   # extended memory
    vita_create_self(jungle.self jungle)
    vita_create_vpk(jungle.vpk ${VITA_TITLEID} jungle.self
        VERSION ${VITA_VERSION}
        NAME ${VITA_APP_NAME})
endif()
