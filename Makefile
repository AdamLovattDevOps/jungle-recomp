# jungle-recomp — native build
#
# Requires SDL2:  brew install sdl2

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter
SDL     := $(shell sdl2-config --cflags)
LIBS    := $(shell sdl2-config --libs) -lm
BIN     := jungle
SRC     := src/jungle.c
DISC    := orig/cd/JUNGLE

.PHONY: all clean verify check test windows e2e hires

all: $(BIN)

$(BIN): $(SRC) src/engine.c src/engine.h src/synth.c src/synth.h src/hires.c src/hires.h src/core.h src/third_party/stb_truetype.h src/blit.c src/gif.c src/timing.c
	$(CC) $(CFLAGS) $(SDL) -o $@ $(SRC) src/engine.c src/synth.c src/hires.c src/blit.c src/gif.c src/timing.c $(LIBS)

# Decode every bitmap in every container, headless.
verify: $(BIN)
	@for f in $(DISC)/*.BIN; do ./$(BIN) $$f --verify || exit 1; done

# Run the script VM and record walker over every container.
check: $(BIN)
	@for f in $(DISC)/*.BIN; do ./$(BIN) $$f --script; done

# Compositor behaviour checks. No disc required.
test:
	@$(CC) $(CFLAGS) -o build/blit_test tests/blit_test.c src/blit.c && ./build/blit_test
	@$(CC) $(CFLAGS) -o build/timing_test tests/timing_test.c src/timing.c && ./build/timing_test

# End-to-end: the game played headless with scripted input (needs the disc data).
e2e: $(BIN)
	@python3 tests/e2e.py $(DISC)

# High-resolution art from your own disc: Real-ESRGAN (realesrgan-ncnn-vulkan, unpacked in
# build/deps/esrgan) upscales every bitmap into build/hires. See docs/PLATFORMS.md.
hires: $(BIN)
	@python3 tools/upscale.py --disc $(DISC)

# Windows cross-build with MinGW-w64; SDL2's MinGW dev package unpacked in build/deps.
WIN_SDL ?= build/deps/SDL2-2.30.9/x86_64-w64-mingw32
windows:
	@mkdir -p build/win64
	x86_64-w64-mingw32-gcc -O2 -Wall -Wno-unused-parameter -I$(WIN_SDL)/include/SDL2 -Dmain=SDL_main \
	  -o build/win64/jungle.exe src/jungle.c src/engine.c src/synth.c src/hires.c src/blit.c src/gif.c src/timing.c \
	  -L$(WIN_SDL)/lib -lmingw32 -lSDL2main -lSDL2 -lm -mwindows
	cp $(WIN_SDL)/bin/SDL2.dll build/win64/

clean:
	rm -f $(BIN) build/blit_test build/timing_test

# One-command status across the whole disc.
report: $(BIN)
	@echo "=== jungle-recomp status ==="
	@printf "bitmaps      "
	@for f in $(DISC)/*.BIN; do ./$(BIN) $$f --verify; done | \
	  awk '{b+=$$2; d+=$$4; f+=$$6} END {printf "%d total, %d decoded, %d failed\n", b, d, f}'
	@printf "audio        "
	@for f in $(DISC)/*.BIN; do ./$(BIN) $$f --audio; done | \
	  awk '/clips decoded/ {n+=$$1; s+=$$6} END {printf "%d clips, %.0f seconds\n", n, s}'
	@printf "scene walker "
	@for f in $(DISC)/*.BIN; do ./$(BIN) $$f --script; done | \
	  sed -E 's/.* ([0-9]+) complete \/ *([0-9]+) partial.*/\1 \2/' | \
	  awk '{c+=$$1; p+=$$2} END {printf "%d complete / %d partial (%.1f%%)\n", c, p, 100*c/(c+p)}'
	@printf "script VM    "
	@for f in $(DISC)/*.BIN; do ./$(BIN) $$f --script; done | \
	  awk '{e+=$$4; f+=$$6} END {printf "%d expressions, %d failures\n", e, f}'
