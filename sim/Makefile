# Host build of the exact firmware UI translation units.
GFX  := ../firmware/components/beacon_gfx
UI   := ../firmware/components/beacon_ui
SRCS := sim_main.cc \
        $(GFX)/beacon_canvas.cc $(GFX)/beacon_gray.cc \
        $(GFX)/generated/beacon_fonts.c \
        $(UI)/beacon_ui.cc $(UI)/beacon_chrome.cc \
        $(UI)/beacon_screens.cc $(UI)/beacon_quiet.cc
INC  := -I$(GFX)/include -I$(GFX)/generated -I$(UI)/include
# The user profile puts the ESP ULP binutils ahead of /usr/bin; -B pins the
# host assembler so this builds in an interactive shell too.
FLAGS := -B/usr/bin -O2 -Wall -Wextra -Wno-unused-parameter $(INC)

out/beacon-sim: $(SRCS)
	@mkdir -p out
	g++ -std=c++17 $(FLAGS) -x c++ $(SRCS) -o $@

.PHONY: run clean
run: out/beacon-sim
	@./out/beacon-sim out 2

clean:
	rm -rf out/beacon-sim out/*.png
