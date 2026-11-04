#---------------------------------------------------------------------------------
# Paper Mario 3DS Homebrew - Makefile (devkitPro Template)
#---------------------------------------------------------------------------------
# This is the official devkitPro template adapted for our project.
#---------------------------------------------------------------------------------

.SUFFIXES:

# The host-tests target builds the developer-side unit-test suite with the
# system cc and runs them with -DPM3DS_HOST_TEST=1; it must work without
# devkitARM. Define it first and short-circuit the devkitARM body of the
# Makefile when it's the goal.
#
# Gated on the configuration view (CURDIR != build): when this Makefile is
# re-entered by the cross-build's recursive `make -C build` invocation,
# host-tests must NOT exist there — otherwise it'd be the first-listed
# target in that scope and steal the implicit default goal from the .3dsx
# / .elf rules.
ifeq ($(filter build build_us build_jp build_ique,$(notdir $(CURDIR))),)
.PHONY: host-tests
host-tests:
	$(MAKE) -C tests run

# Asset / romfs pipeline (milestone 3). Both targets are no-ops in CI
# (where there's no baserom) and on-demand for the developer.
#
# Usage:
#   make -C port/3ds assets BASEROM=/path/to/baserom.z64
#   make -C port/3ds romfs
#   make -C port/3ds PM3DS_ENABLE_ROMFS=1   # produce .3dsx with embedded romfs
.PHONY: assets romfs
assets:
ifndef BASEROM
	$(error BASEROM=/path/to/baserom.z64 required for `make assets`)
endif
	python3 tools/extract_assets.py "$(BASEROM)"

romfs: tools/pack_romfs.py
	python3 tools/pack_romfs.py
endif

ifeq ($(filter host-tests,$(MAKECMDGOALS)),)

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment.")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
# which ROM the build is for: pal = Paper Mario (Europe), us = Paper Mario (USA),
# jp = Mario Story (Japan), ique = the iQue Player's Chinese release
# (make PM3DS_VERSION=us / jp / ique). Each version has its own build directory, output
# name and generated files (build_assets*, world_files*.mk, m11_syms*.ld);
# include/pm3ds_rom_layout.h holds its ROM offsets.
PM3DS_VERSION	?=	pal
ifeq ($(PM3DS_VERSION),pal)
PM3DS_VFLAG	:=	-DVERSION_PAL=1
PM3DS_VSUFFIX	:=
else ifeq ($(PM3DS_VERSION),us)
PM3DS_VFLAG	:=	-DVERSION_US=1
PM3DS_VSUFFIX	:=	_us
else ifeq ($(PM3DS_VERSION),jp)
PM3DS_VFLAG	:=	-DVERSION_JP=1
PM3DS_VSUFFIX	:=	_jp
else ifeq ($(PM3DS_VERSION),ique)
PM3DS_VFLAG	:=	-DVERSION_IQUE=1
PM3DS_VSUFFIX	:=	_ique
else
$(error PM3DS_VERSION must be pal, us, jp or ique)
endif
TARGET		:=	PaperMario3DS$(PM3DS_VSUFFIX)
BUILD		:=	build$(PM3DS_VSUFFIX)
# the name players see (include/pm3ds_brand.h, cia/app.rsf, tools/make_branding.py)
APP_TITLE	:=	Star Haven
APP_DESCRIPTION	:=	Paper Mario port for New 3DS (bring your own ROM)
APP_AUTHOR	:=	Star Haven team
ICON		:=	cia/icon.png

#---------------------------------------------------------------------------------
# DO NOT add the following decomp directories to SOURCES:
#   ../../src/os/         (decompiled libultra — our n64_shim/ replaces these)
#   ../../src/os/nusys/   (decompiled nusys —     our n64_shim/ replaces these)
#   ../../src/buffers.c   (defines N64 framebuffer addresses — we manage CFBs)
# Adding any of these will produce multiple-definition link errors against the
# shim layer. See docs/superpowers/specs/2026-05-13-papermario-3ds-boot-hal-design.md
# (HAL surface inventory section).
#---------------------------------------------------------------------------------
# Milestones 1+2 build the port-side files only — no ../../src/ (decomp).
# The decomp tree pulls in generated headers (icon_offsets.h ICON_* enums,
# item_data.inc.c, etc.) that no current milestone has a producer for.
# Asset codegen lands in milestone 3 (asset extraction + romfs pipeline);
# compile fidelity of the decomp tree lands in milestone 4.
#
# source/n64_shim/ now contains the F3DEX2 → PICA200 translator family
# alongside the boot-HAL shims:
#   gfx_dl.c        — display-list submit queue (FAITHFUL)
#   gfx_translate.c — dispatch + state mirror + handlers (~30 commands)
#   gfx_geom.c      — matrix stack + Vtx pool conversion
#   gfx_texture.c   — N64 format decoders + LRU cache + morton swizzle
#   gfx_combiner.c  — N64 CC → PICA200 TexEnv table + warn fallback
#   gfx_present.c   — frame loop drains queue and walks each DL
#
# All five new gfx_*.c are picked up by the source/n64_shim wildcard;
# no SOURCES line needs to grow.
SOURCES		:=	source source/platform source/n64_shim \
				../../src/gcc \
				../../src/charset \
				../../src/common \
				../../src/evt \
				../../src/entity \
				../../src/effects \
				../../src/world \
				../../src/battle \
				../../src/pause \
				../../src/filemenu \
				../../src/audio \
				../../src \
				../../src/bss
# M4.2 wave D attempted to add src/battle/{action_cmd,common,move,partner},
# src/world/{action,common,partner,model_anim,script_api}, src/audio/core
# to satisfy boot_main's transitive function-pointer table references.
# That bring-in triggered basename collisions in OFILES
# (src/world/action/jump.c vs src/battle/action_cmd/jump.c → both jump.o)
# that need namespaced .o paths to resolve cleanly. Reverted; the boot_main
# wire is M5+ work. Wave B's ROM-segment stubs landed via
# port/3ds/source/n64_shim/m42_rom_stubs.c (1939 weak storage stubs covering
# 642 segment basenames × 3 markers each) and remain useful for that
# future work.
#
# (Old wave-13 deferral comment below — kept for reference.)
# M4 wave 13 deferred: src/bss requires NUSched, OSPi{Handle,fRam}, ALHeap,
# PFS_*, NUContRmbCtl, NU_*, BLOCKSIZE, OS_NUM_EVENTS and ~30 more
# N64-internal types/constants that the port doesn't shim. The BSS files
# allocate storage for these globals; without the headers the file fails to
# parse. Compiling src/bss is an M5+ goal (real game state alignment).
# Note: extern decls in other src/ files that reference these symbols won't
# link if their use sites are reached at runtime, but M4's goal is compile
# fidelity, not link fidelity for unused subsystems.
DATA		:=	data
# gen/<version>: headers generated per ROM version (tools/gen_version_headers.sh;
# the PAL ones are in include/), ahead of the shared copies
INCLUDES	:=	gen/$(PM3DS_VERSION) include include/n64 include/nu include/sprite \
			../../include \
			../../src \
			../../src/audio \
			../../assets/$(PM3DS_VERSION) \
			../../include/battle \
			../../include/world/common \
			../../include/world/actions \
			../../include/world/partners \
			../../include/world \
			../../include/charset \
			../../include/starpoint \
			../../include/level_up \
			../../include/entity \
			../../include/npc \
			../../include/pause \
			../../include/filemenu \
			../../include/overlay \
			../../include/effects \
			../../include/message_ids \
			../../include/battle/common \
			../../include/battle/move \
			../../include/battle/actor \
			../../include/battle/area

#---------------------------------------------------------------------------------
# CFLAGS controls compiler options
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS	:=	-g -Wall -O2 -mword-relocations \
			-ffunction-sections -fdata-sections \
			$(ARCH)

CFLAGS	+=	-include include_asm.h $(INCLUDE) -D_3DS -D__3DS__ -DARM11 $(PM3DS_VFLAG) -DNON_MATCHING=1 $(EXTRA_CFLAGS)

# M9: a failed decomp ASSERT() must not be a silent infinite loop. Without this
# (and without DEBUG, which the port does not define -- src/is_debug.c is in
# M4_EXCLUDE below) include/macros.h expands IS_DEBUG_PANIC to
# `do {} while(true)`, which is how the title-screen hang presented: frozen main
# thread, no message at all. This routes it to pm3ds_assert_failed()
# (source/n64_shim/m9_assert_shim.c), which logs statement/file/line.
CFLAGS	+=	-DPM3DS_ASSERT_LOG=1

# M10: real map shapes are ~0x14000 bytes but include/model.h nominally sizes
# gMapShapeData at SHAPE_SIZE_LIMIT 0x8000, so decode_yay0 overruns it by ~48 KB
# on every map load. Measured: the demo map decodes to 0x13AE0 with header
# pointers reaching offset 0x131F0. Port-only enlargement; see model.h.
CFLAGS	+=	-DPM3DS_BIG_MAPSHAPE=1

# M10: with -DSHIFT=1 some ROM offsets resolve to the ADDRESS of a weak linker
# stub instead of a cart offset (the M8.8 mapfs_ROM_START defect). Selects the
# real PAL cart offsets instead. Currently covers SPRITE_ROM_START in
# src/101b90_len_8f0.c; add further sites under the same flag as they surface.
CFLAGS	+=	-DPM3DS_PORT_ROM_ADDRS=1

# M11: the full game build. Before this, the flag set lived only in
# EXTRA_CFLAGS on the command line, and dropping one (SHAPE_RELOC is nested
# inside MAPSHAPE_DBG, player rasters inside SPRITE_TABLE) regressed silently.
# `make PM3DS_GAME=1 PM3DS_ENABLE_ROMFS=1` is the build to test.
ifeq ($(PM3DS_GAME),1)
CFLAGS	+=	-DPM3DS_BOOT_GAME=1 -DPM3DS_INVOKE_RETRACE_CB=1 -DPM3DS_TITLE_DBG=1 \
			-DPM3DS_MAPSHAPE_DBG=1 -DPM3DS_SHAPE_RELOC=1 -DPM3DS_RELOC_DLS=1 \
			-DPM3DS_SPRITE_TABLE=1 -DPM3DS_NATIVE_WORLD=1 -DPM3DS_DMA_TRACE=1 -DPM3DS_AUDIO_ENGINE=1 \
			-DPM3DS_ASSET_ROOT=\"$(TOPDIR)/build_assets$(PM3DS_VSUFFIX)/\"
endif

# M4 wave 12: decomp src/sprite.c contains two `return;` statements in
# non-void s32-returning functions, with the inline comment "bug: does not
# return a value". Newer gcc treats this as an error; downgrade to warning
# so the decomp source builds as-is without us touching the upstream.
CFLAGS	+=	-Wno-error=return-mismatch -Wno-return-mismatch

# M4.1: enable -fcommon so tentative definitions in src/ root files
# (e.g. `s32 PeachDisguiseNpcIndex;` in src/7B440.c) merge with the
# storage allocations in src/bss/ (`BSS s32 PeachDisguiseNpcIndex;`).
# Modern gcc 10+ defaults to -fno-common which makes both forms real
# definitions, producing multiple-definition link errors. Upstream
# papermario relied on the historical -fcommon default.
CFLAGS	+=	-fcommon

# M14.4: keep top-level variables in source order. N64 code relies on
# consecutive declarations being adjacent (palette+image blobs decoded into
# the first array, heap regions borrowed past their declared size); GCC
# otherwise emits a TU's variables in reverse order.
CFLAGS	+=	-fno-toplevel-reorder

# M6.1: -DSHIFT=1 unconditionally. SHIFT is the decomp's escape hatch
# for builds that aren't producing a byte-exact N64 ROM (which is us).
# It:
#   - turns load_obfuscation_shims() into a no-op (the N64 build
#     dynamically DMA-loads anti-piracy code from cart ROM to a
#     specific VRAM address, then jumps there; on 3DS that's both
#     impossible and unnecessary)
#   - macro-redirects shim_create_audio_system_obfuscated and
#     shim_load_engine_data_obfuscated to call create_audio_system /
#     load_engine_data directly (bypassing the obfuscation indirection)
#   - enables _with_shift variants of audio + draw_box helpers that
#     don't rely on the obfuscation layer
# Without PM3DS_BOOT_GAME, boot_main is garbage-collected by the
# linker, so SHIFT has no effect; with PM3DS_BOOT_GAME set, SHIFT is
# what keeps boot_main from immediately calling indirect function
# pointers into a zero-initialized stub VRAM area.
CFLAGS	+=	-DSHIFT=1

CXXFLAGS	:=	$(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map) \
				-Wl,--wrap=load_engine_data \
				-Wl,--wrap=state_step_startup \
				-Wl,--wrap=state_step_logos \
				-Wl,--wrap=state_drawUI_logos \
				-Wl,--wrap=fio_load_globals \
				-Wl,--wrap=decode_yay0 \
				-Wl,--wrap=state_step_title_screen \
				-Wl,--wrap=appendGfx_title_screen \
				-Wl,--wrap=set_background \
				-Wl,--wrap=C3D_UpdateUniforms

LIBS	:=	-lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lcitro3d -lctru -lm

#---------------------------------------------------------------------------------
# libctru path
#---------------------------------------------------------------------------------
LIBDIRS	:=	$(PORTLIBS) $(CTRULIB)

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

# Pin the default goal in the configuration branch so adding the
# host-tests phony at the top of this file (which would otherwise be the
# first-listed target) doesn't steal it. Inside this branch `all` is
# defined below; the recursive make in the build/ dir takes the else
# branch and uses its own first-target default (the .3dsx rule).
.DEFAULT_GOAL := all

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(filter-out %.inc.c,$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c))))
# M4 wave 12: exclude decomp src/ root files that pull in N64-specific
# libultra internals we don't ship on 3DS. The port-side n64_shim layer
# replaces these.
M4_EXCLUDE := crash_screen.c create_audio_system_obfuscated.c is_debug.c load_engine_data_obfuscated.c
# iQue's splat has no *_obfuscated segments, so the wrappers for them can't compile
# (their callers are native here: see SHIFT above). From Darth-Koopa's iQue fork.
ifeq ($(PM3DS_VERSION),ique)
M4_EXCLUDE += obfuscation_shims.c
endif
# files only the PAL ROM has (ver/pal/splat.yaml but not ver/us): languages,
# the title-screen language menu, PAL heap layout
PAL_ONLY_CFILES := filemenu_selectlanguage.c heaps4.c titlemenu.c \
	level_up_letters_en_de.c level_up_letters_es.c level_up_letters_fr.c \
	pause_gfx_de.c pause_gfx_en.c pause_gfx_es.c pause_gfx_fr.c \
	starpoint_de.c starpoint_en.c starpoint_es.c starpoint_fr.c
ifneq ($(PM3DS_VERSION),pal)
M4_EXCLUDE += $(PAL_ONLY_CFILES)
endif
CFILES		:=	$(filter-out $(M4_EXCLUDE),$(CFILES))
# Wave 12: deduplicate after the $(notdir ...) wildcard. Historical:
# port/3ds/source/main.c and src/main.c both produced main.o by basename.
# M6.1: port-local file renamed to port_main.c so both can coexist;
# this $(sort) remains useful as a general dedup-safety net for any
# future SOURCES additions.
CFILES		:=	$(sort $(CFILES))
# decomp files (not the port's own source/ dirs) get the N64 data layout,
# see WORLD_DATA_FLAGS below
PORT_CFILES	:=	$(foreach dir,source source/platform source/n64_shim,$(notdir $(wildcard $(dir)/*.c)))
export DECOMP_OFILES := $(patsubst %.c,%.o,$(filter-out $(PORT_CFILES),$(CFILES)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
PICAFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.v.pica)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(PICAFILES)),)
SHBINS		:=
else
SHBINS		:=	$(PICAFILES:.v.pica=.shbin)
endif

#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
#---------------------------------------------------------------------------------
	export LD	:=	$(CC)
#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
	export LD	:=	$(CXX)
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

export OFILES_SOURCES  := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES_BIN      := $(addsuffix .o,$(BINFILES)) $(addsuffix .o,$(SHBINS))
export OFILES := $(OFILES_BIN) $(OFILES_SOURCES)

export HFILES  := $(addsuffix .h,$(subst .,_,$(BINFILES))) $(addsuffix .h,$(subst .,_,$(SHBINS)))

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export _3DSXDEPS := $(if $(NO_SMDH),,$(OUTPUT).smdh)

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.png) $(wildcard $(TOPDIR)/*.png)
	ifneq (,$(findstring $(TARGET).png,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).png
	else
		ifneq (,$(findstring icon.png,$(icons)))
			export APP_ICON := $(TOPDIR)/icon.png
		else
			export APP_ICON := $(CTRULIB)/default_icon.png
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_SMDH)),)
	export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh
endif

ifeq ($(strip $(ROMFS)),)
else
	export _3DSXFLAGS += --romfs=$(CURDIR)/$(ROMFS)
endif

.PHONY: all clean

#---------------------------------------------------------------------------------
all: $(BUILD) $(DEPSDIR)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
	@echo "checking undefined symbols..."
	@python3 $(TOPDIR)/tools/check_undef_syms.py $(TARGET).elf

$(BUILD):
	@mkdir -p $@

ifneq ($(DEPSDIR),$(BUILD))
$(DEPSDIR):
	@mkdir -p $@
endif

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(OUTPUT).smdh $(TARGET).elf

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------

DEPENDS := $(OFILES:.o=.d)

# Milestone-3 romfs embedding. The default $(OUTPUT).3dsx recipe lives
# in 3ds_rules and is `3dsxtool $< $@ $(_3DSXFLAGS)`. We append
# `--romfs $(TOPDIR)/romfs` to _3DSXFLAGS when PM3DS_ENABLE_ROMFS=1.
# 3dsxtool packs everything under that directory into the .3dsx as
# embedded romfs accessed at runtime via `romfs:/<path>`.
ifeq ($(PM3DS_ENABLE_ROMFS),1)
_3DSXFLAGS += --romfs=$(TOPDIR)/romfs
endif

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
$(OUTPUT).3dsx	: $(OUTPUT).elf $(_3DSXDEPS)

$(OFILES_SOURCES) : $(HFILES)

$(OUTPUT).elf	: $(OFILES)

# M11: natively compiled world maps (see tools/gen_world_files.py). Objects
# keep their directory under wobj/ because every map has a main.c/npc.c.
ifeq ($(PM3DS_GAME),1)
include $(TOPDIR)/world_files$(PM3DS_VSUFFIX).mk
WORLD_OFILES := $(addprefix wobj/,$(WORLD_CFILES:.c=.o))
OFILES += $(WORLD_OFILES)
# effect shim aliases + zeroed overlay address symbols (tools/gen_link_syms.py)
LDFLAGS += $(TOPDIR)/m11_syms$(PM3DS_VSUFFIX).ld
$(OUTPUT).elf : $(WORLD_OFILES)
# One data section per file, as in the N64 build: display lists that end
# without gsSPEndDisplayList fall through into the next array (fire_breath's
# D_09000B88_373F68 -> D_09000BA8_373F88), and animations read frames past the
# array they name. With -fdata-sections the linker drops the unnamed array.
WORLD_DATA_FLAGS := -fno-data-sections
$(DECOMP_OFILES): CFLAGS += $(WORLD_DATA_FLAGS)
wobj/%.o : $(TOPDIR)/../../src/%.c
	@mkdir -p $(dir $@)
	@echo $*.c
	$(SILENTCMD)$(CC) -MMD -MP -MF $(@:.o=.d) $(_EXTRADEFS) $(CPPFLAGS) $(CFLAGS) $(WORLD_DATA_FLAGS) -c $< -o $@ $(ERROR_FILTER)
-include $(WORLD_OFILES:.o=.d)
endif

%.bin.o	%_bin.h : %.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

%.shbin.o %_shbin.h : %.shbin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)


#---------------------------------------------------------------------------------
-include $(DEPSDIR)/*.d
#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------

endif # filter host-tests — closes the goal-based devkitARM gate at the top of this file
