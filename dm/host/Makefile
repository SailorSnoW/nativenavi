# Game profile (dm/games/<id>/): Ocarina of Time's unless GAME names a derived profile's title ID. game_profile.h is generated from its game.tsv into gen/<id>/
GAME ?= 0400486024DEC000
# A profile outside the repository (DM_GAMES_DIR) is reached through the symlink gen/<id>/profile: make cannot take paths with spaces.
# DM_GAMES_FIRST=1 takes it even when dm/games has the game
ifneq ($(strip $(GAME)),)
GAME_DIR := $(if $(and $(wildcard ../games/$(GAME)/game.tsv),$(if $(DM_GAMES_FIRST),,1)),../games/$(GAME),$(shell python3 ../games/game.py link $(GAME) gen/$(GAME)/profile >&2 && echo gen/$(GAME)/profile))
ifeq ($(GAME_DIR),)
$(error no game profile $(GAME) (neither in dm/games nor in DM_GAMES_DIR))
endif
endif
GAME_H = gen/$(GAME)/game_profile.h
include ../mac/min_os.mk
# Not -O3 (no measured gain over -O2) and not -flto, which moves hle_bigstack.c's inline-assembly entry apart from its return path (SIGBUS in dm_bigstack_enter)
CFLAGS=-O2 -g -Wall -mmacosx-version-min=$(MACOS_MIN)
# One job per core unless the command line sets -j (GNU make 3.81, the system's, takes -j from MAKEFLAGS set here)
ifeq ($(filter -j%,$(MAKEFLAGS)),)
MAKEFLAGS += -j$(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
endif
# hle_*.c by wildcard, so that adding one does not touch this line
SRC=main.c loader.c hle.c crash.c probe.c thread.c gamedata.c presence.c memstat.m haptics_apple.m audio_input.c audio_input_mac.m audio_input_dualsense_mac.m dualsense_mic_mac.c pad_switch2_apple.m $(sort $(wildcard hle_*.c)) front_mac.m entry.S
# NVN, nn::vi and the Metal layer (dm/gfx, nvn and tex; not tests/ or metal/)
GFX=$(sort $(wildcard ../gfx/*.c ../gfx/nvn/*.c ../gfx/nvn/*.m ../gfx/nvn/*.S ../gfx/tex/*.c ../gfx/tex/*.m))
# dav1d, the AV1 decoder where VideoToolbox has none (hle_movie_av1.c). Its *_tmpl.c files are built through the per-bit-depth wrappers in config/tmpl, and the assembly
# files other assembly files include (*_tmpl.S, util.S) are not built on their own: the list meson builds
DAV1D=../third_party/dav1d
DAV1D_SRC=$(sort $(filter-out %_tmpl.c,$(wildcard $(DAV1D)/src/*.c)) $(DAV1D)/src/arm/cpu.c $(wildcard $(DAV1D)/config/tmpl/*.c) \
          $(filter-out %_tmpl.S %/util.S,$(wildcard $(DAV1D)/src/arm/64/*.S)))
# An object is named by its source's path (src/arm/cpu.c: gen/dav1d/src_arm_cpu.c.o): an archive keeps only file names, and with two cpu.c.o dsymutil skips the second's
# debug information
dav1d_obj=gen/dav1d/$(subst /,_,$(patsubst $(DAV1D)/%,%,$(1))).o
DAV1D_OBJ=$(foreach s,$(DAV1D_SRC),$(call dav1d_obj,$(s)))
DAV1D_LIB=gen/dav1d/libdav1d.a
DAV1D_H=$(wildcard $(DAV1D)/config/*.h $(DAV1D)/src/*.h $(DAV1D)/src/*_tmpl.c $(DAV1D)/src/arm/*.h $(DAV1D)/src/arm/asm.S $(DAV1D)/src/arm/64/*_tmpl.S $(DAV1D)/src/arm/64/util.S $(DAV1D)/include/*/*.h)
define DAV1D_RULE
$(call dav1d_obj,$(1)): $(1) $$(DAV1D_H) ../mac/min_os.mk
	@mkdir -p $$(dir $$@)
	clang -O2 -g -mmacosx-version-min=$$(MACOS_MIN) -fvisibility=hidden -fomit-frame-pointer -fno-align-functions -I$$(DAV1D) -I$$(DAV1D)/include -I$$(DAV1D)/config -c $$< -o $$@
endef
$(foreach s,$(DAV1D_SRC),$(eval $(call DAV1D_RULE,$(s))))
$(DAV1D_LIB): $(DAV1D_OBJ)
	rm -f $@; ar rcs $@ $^
# The host's Rust code, a static library (rust.mk: RS_LIB)
include rust.mk
.PHONY: rust
rust: $(RS_LIB)
# The host's own Metal shaders (dm/gfx/msl/*.metal), compiled for MSL_SDK into gen/msl-<sdk>/host_msl_gen.h, so the host compiles no MSL at run time
MSL_SDK ?= macosx
MSL_MIN_macosx = $(MACOS_MIN)
MSL_SRC = $(sort $(wildcard ../gfx/msl/*.metal))
MSL_H = gen/msl-$(MSL_SDK)/host_msl_gen.h
# Run on every build (the toolchain may change); the generator leaves the header as it is when its inputs match
$(MSL_H): $(MSL_SRC) ../gfx/tools/host_msl.py FORCE
	python3 ../gfx/tools/host_msl.py --sdk $(MSL_SDK) $(if $(MSL_MIN_$(MSL_SDK)),--min $(MSL_MIN_$(MSL_SDK))) $@ $(MSL_SRC)
.PHONY: host_msl
host_msl: $(MSL_H)
.DEFAULT_GOAL := dmhost   # the header rules come first
.PHONY: game_profile
game_profile: $(GAME_H)
# FORCE: the same GAME can come from another profile folder (DM_GAMES_FIRST), whose game.tsv may be older than the header; game.py writes it only when it changes
$(GAME_H): $(GAME_DIR)/game.tsv ../games/game.py FORCE
	@python3 ../games/game.py header $(GAME) $@
.PHONY: FORCE
FORCE:
# The host: one object per source in gen/<id>/obj (dm.h includes the profile header, so every game has its own), linked into gen/<id>/dmhost with the same flags (as
# clang's one-step compile and link would) and its dSYM made from the objects. -MMD -MP: each object depends on the headers its source read. The generated headers are
# order-only prerequisites: the dependency files name them for the sources that read them
HOST_CFLAGS = $(CFLAGS) -I../gfx/nvn/include -I$(dir $(MSL_H)) -I$(dir $(GAME_H)) -I$(DAV1D)/include -fobjc-arc
HOST_FRAMEWORKS = -framework Cocoa -framework UserNotifications -framework QuartzCore -framework Metal -framework MetalFX -framework GameController -framework CoreBluetooth -framework IOBluetooth -framework CoreHaptics -framework IOKit -framework IOUSBHost -framework AudioToolbox -framework CoreAudio -framework AVFAudio -framework CoreFoundation -framework VideoToolbox -framework CoreMedia -framework CoreVideo -framework ImageIO -framework UniformTypeIdentifiers
HOST_SRC = $(SRC) $(GFX)
ifneq ($(filter ../%,$(filter-out ../gfx/%,$(HOST_SRC))),)
$(error host sources outside dm/host and dm/gfx need an object rule: $(filter ../%,$(filter-out ../gfx/%,$(HOST_SRC))))
endif
OBJ_DIR = gen/$(GAME)/obj
HOST_OBJ = $(foreach s,$(HOST_SRC),$(if $(filter ../gfx/%,$(s)),$(OBJ_DIR)/gfx/$(s:../gfx/%=%).o,$(OBJ_DIR)/host/$(s).o))   # the line's order: the link lays code out in it
HOST_BIN = gen/$(GAME)/dmhost
$(OBJ_DIR)/host/%.o: % Makefile ../mac/min_os.mk | $(GAME_H) $(MSL_H)
	@mkdir -p $(dir $@)
	clang $(HOST_CFLAGS) -MMD -MP -c $< -o $@
$(OBJ_DIR)/gfx/%.o: ../gfx/% Makefile ../mac/min_os.mk | $(GAME_H) $(MSL_H)
	@mkdir -p $(dir $@)
	clang $(HOST_CFLAGS) -MMD -MP -c $< -o $@
$(HOST_BIN): $(HOST_OBJ) $(DAV1D_LIB) $(RS_LIB) Makefile rust.mk ../mac/min_os.mk
	clang $(HOST_CFLAGS) -o $@ $(HOST_OBJ) $(DAV1D_LIB) $(RS_LIB) $(HOST_FRAMEWORKS)
	dsymutil $@
ifneq ($(strip $(GAME)),)
-include $(HOST_OBJ:.o=.d)
endif
# dm/host/dmhost (what the run commands, regress.py and CI start) is a copy of this game's gen/<id>/dmhost with its dSYM, made again whenever the two differ (make's
# whole-second times cannot tell two builds made in one second apart)
dmhost: $(HOST_BIN) FORCE
	@cmp -s $(HOST_BIN) $@ || { rm -rf $@ $@.dSYM && cp $(HOST_BIN) $@ && cp -R $(HOST_BIN).dSYM $@.dSYM; }
clean:   # everything make generates for the host
	rm -rf gen dmhost dmhost_* *.dSYM
.PHONY: clean
