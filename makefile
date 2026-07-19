UNAME_S := $(shell uname -s)
# use "make" to build for linux
# use "make debug" or "make DEBUG=TRUE" to build a debug executable for linux
# use "make ARCH=win32" or "make ARCH=win64" to build for windows
# Interesting targets (in addition to the default 'all'):
#  - debug
#  - clean
#  - distrib
#  - doc
# Supported variables:
#  - ARCH = (linux|win32|win64|macos)
#  - CXX (default = g++)
#  - CFLAGS
#  - LDFLAGS
#  - WITHOUT_GL

# To be overridden for debian packaging
VERSION=latest
REVISION=0

LAST_BUILD_IN_DEBUG = $(shell [ -e .debug ] && echo 1 || echo 0)

ifeq ($(UNAME_S),Darwin)
ARCH ?= macos
else
ARCH ?= linux
endif

COMMON_CFLAGS ?= 

USE_VENDORED_SDL ?= 1
SDL_VENDOR_DIR = vendor/SDL
SDL_VENDOR_BUILD = $(SDL_VENDOR_DIR)/install
SDL_VENDOR_INCLUDE = -I$(SDL_VENDOR_DIR)/include
SDL_VENDOR_LIBS = -L$(SDL_VENDOR_BUILD)/lib -lSDL3

ifeq ($(ARCH),win64)
# Rename main to SDL_main to solve the "undefined reference to `SDL_main'".
# Do not make an error of old-style-cast or zero-as-null-pointer-constant on
# msys2 as vendor headers trigger these warnings.
COMMON_CFLAGS = -DWINDOWS -D_POSIX_C_SOURCE=200809L
WARN_SUPPRESS = -Wno-error=old-style-cast -Wno-error=zero-as-null-pointer-constant
PLATFORM=windows
MINGW_PATH=/mingw64
else ifeq ($(ARCH),win32)
COMMON_CFLAGS = -DWINDOWS -D_POSIX_C_SOURCE=200809L
WARN_SUPPRESS = -Wno-error=old-style-cast -Wno-error=zero-as-null-pointer-constant
PLATFORM=windows
MINGW_PATH=/mingw32
else ifeq ($(ARCH),linux)
PLATFORM=linux
else ifeq ($(ARCH),macos)
# Yes that's weird, but the build on macos works the same way as on linux
PLATFORM=linux
WARN_SUPPRESS = -Wno-error=old-style-cast -Wno-error=zero-as-null-pointer-constant -Wno-error=missing-braces -Wno-error=deprecated-declarations -Wno-error=self-assign -Wno-error=vla-cxx-extension
LDFLAGS += -framework Cocoa
else
$(error Unknown ARCH. Supported ones are linux, win32 and win64.)
endif

# macOS code signing macro: $(1) = target, $(2) = extra flags (e.g. --deep)
define SIGN_MACOS
@xattr -cr $(1) 2>/dev/null || true
@if security find-identity -v -p codesigning | grep -q "Developer ID Application"; then \
	codesign --force $(2) --options runtime -s "Developer ID Application" $(1); \
	echo "Signed $(1) with Developer ID"; \
else \
	codesign --force $(2) -s - $(1); \
	echo "Ad-hoc signed $(1)"; \
fi
endef

ifeq ($(PLATFORM),windows)
TARGET = koncepcja.exe
TEST_TARGET = test_runner.exe
COMMON_CFLAGS += -DWINDOWS
else
prefix = /usr/local
TARGET = koncepcja
TEST_TARGET = test_runner
endif

PKG_SDL_CFLAGS=`pkg-config --cflags sdl3`
PKG_SDL_LIBS=`pkg-config --libs sdl3`
# SDL_image optional support removed; PNG loads via libpng
# Resolve the vendored SDL via WORKSPACE-RELATIVE paths, not pkg-config.
# pkg-config reads the install's sdl3.pc, whose baked-in ABSOLUTE prefix breaks
# whenever the checkout moves: the konCePCja -> koncepcja_v5 repo rename left a
# stale cached prefix (/home/runner/work/konCePCja/...) that no longer exists on
# the case-sensitive Linux runner, so pkg-config --cflags sdl3 returned a bogus
# -I and every SDL3/*.h include failed (the Linux + Coverage CI red). The
# submodule headers and the built lib are always at these workspace-relative
# locations, immune to renames. USE_VENDORED_SDL=0 opts back into pkg-config.
ifeq ($(USE_VENDORED_SDL),1)
PKG_SDL_CFLAGS=$(SDL_VENDOR_INCLUDE)
PKG_SDL_LIBS=$(SDL_VENDOR_LIBS)
ifeq ($(ARCH),macos)
LDFLAGS += -Wl,-rpath,@executable_path/$(SDL_VENDOR_BUILD)/lib
endif
ifeq ($(ARCH),linux)
LDFLAGS += -Wl,-rpath,'$$ORIGIN/$(SDL_VENDOR_BUILD)/lib'
endif
endif
IPATHS = -Isrc/ -isystem vendor/imgui -isystem vendor/imgui/backends -isystem vendor/msf_gif -isystem vendor/ImGuiColorTextEdit -isystem vendor/portable-file-dialogs `pkg-config --cflags freetype2` $(PKG_SDL_CFLAGS) `pkg-config --cflags libpng` `pkg-config --cflags zlib`
LIBS = $(PKG_SDL_LIBS) `pkg-config --libs freetype2` `pkg-config --libs libpng` `pkg-config --libs zlib`
ifeq ($(PLATFORM),windows)
LIBS += -lws2_32 -luuid -lwinmm
else ifeq ($(ARCH),linux)
endif
CXX ?= g++
COMMON_CFLAGS += -fPIC

# Optional libjpeg support for AVI recording
# jpeg-turbo on macOS is keg-only, so check both default and homebrew paths
BREW := $(shell which brew 2>/dev/null)
ifneq ($(BREW),)
JPEG_PREFIX := $(shell $(BREW) --prefix jpeg-turbo 2>/dev/null)
ifeq ($(JPEG_PREFIX),)
JPEG_PREFIX := $(shell $(BREW) --prefix libjpeg-turbo 2>/dev/null)
endif
endif
ifneq ($(JPEG_PREFIX),)
HAS_LIBJPEG := $(shell test -f $(JPEG_PREFIX)/include/jpeglib.h && echo 1)
ifeq ($(HAS_LIBJPEG),1)
COMMON_CFLAGS += -DHAS_LIBJPEG -I$(JPEG_PREFIX)/include
LIBS += -L$(JPEG_PREFIX)/lib -ljpeg
endif
else
# Fallback: test whether the compiler can actually find and preprocess jpeglib.h
# Use printf with octal \043 for '#' to avoid shell escaping issues
HAS_LIBJPEG := $(shell printf '\043include <jpeglib.h>' | $(CXX) -x c++ -E - >/dev/null 2>&1 && echo 1)
ifeq ($(HAS_LIBJPEG),1)
COMMON_CFLAGS += -DHAS_LIBJPEG
LIBS += -ljpeg
endif
endif

ifneq (,$(findstring g++,$(CXX)))
LIBS += -lstdc++fs
endif

# libcurl for M4 Board HTTP support (skip on Windows to avoid DLL bloat)
ifneq ($(PLATFORM),windows)
HAS_LIBCURL := $(shell pkg-config --exists libcurl 2>/dev/null && echo 1 || (curl-config --libs >/dev/null 2>&1 && echo 1))
ifeq ($(HAS_LIBCURL),1)
COMMON_CFLAGS += -DHAS_LIBCURL $(shell pkg-config --cflags libcurl 2>/dev/null || curl-config --cflags 2>/dev/null)
LIBS += $(shell pkg-config --libs libcurl 2>/dev/null || curl-config --libs 2>/dev/null)
endif
endif

ifeq ($(UNAME_S),Darwin)
ifeq ($(ARCH),)
ARCH=macos
endif
endif

ifndef RELEASE
GIT_HASH = $(shell git rev-parse --verify HEAD)
COMMON_CFLAGS += -DHASH=\"$(GIT_HASH)\"
endif

# Single source of truth for the binary version string.  Mirror the same
# value in CMakeLists.txt's project(... VERSION) — that one drives CPack
# / installer metadata.  See chore: keep-version-in-sync if it ever drifts.
KONCPC_VERSION := $(shell cat VERSION 2>/dev/null)
ifneq ($(KONCPC_VERSION),)
COMMON_CFLAGS += -DKONCPC_VERSION_STRING=\"v$(KONCPC_VERSION)\"
endif

ifdef APP_PATH
COMMON_CFLAGS += -DAPP_PATH=\"$(APP_PATH)\"
else
$(info Notice: APP_PATH not specified.  Will look for koncepcja.cfg debug-style.  See `README.md` for details. )
endif

ifdef DESTDIR
COMMON_CFLAGS += -DDESTDIR=\"$(DESTDIR)\"
endif

CLANG_FORMAT=clang-format
CLANG_TIDY=clang-tidy
CLANG_CHECKS=modernize-*,performance-*,misc-*,readability-*,-misc-definitions-in-headers,-misc-non-private-member-variables-in-classes,-misc-no-recursion,-modernize-avoid-c-arrays,-modernize-deprecated-headers,-modernize-make-unique,-modernize-use-auto,-modernize-use-default-member-init,-modernize-use-equals-delete,-modernize-use-nodiscard,-modernize-use-trailing-return-type,-modernize-use-using,-performance-unnecessary-value-param,-readability-avoid-const-params-in-decls,-readability-braces-around-statements,-readability-convert-member-functions-to-static,-readability-function-size,-readability-implicit-bool-cast,-readability-implicit-bool-conversion,-readability-isolate-declaration,-readability-magic-numbers,-readability-qualified-auto,-readability-uppercase-literal-suffix,-readability-use-anyofallof

SRCDIR:=src
TSTDIR:=test
OBJDIR:=obj/$(ARCH)
RELEASE_DIR = release
ARCHIVE = koncepcja-$(ARCH)
ARCHIVE_DIR = $(RELEASE_DIR)/$(ARCHIVE)

HTML_DOC:=doc/man.html
GROFF_DOC:=doc/man6/koncepcja.6

MAIN:=$(OBJDIR)/main.o

SOURCES:=$(shell find $(SRCDIR) -name \*.cpp)
MM_SOURCES:=
ifeq ($(ARCH),macos)
MM_SOURCES:=$(shell find $(SRCDIR) -name \*.mm)
endif
HEADERS:=$(shell find $(SRCDIR) -name \*.h)

# Vendored / third-party files — excluded from clang-format and clang-tidy.
# msf_gif.h, TextEditor.{h,cpp}, LanguageDefinitions.cpp and
# portable-file-dialogs.h live under vendor/ now (relocation sweep,
# 2026-07-13) so $(SOURCES)/$(HEADERS) (which only walk $(SRCDIR)) never see
# them in the first place; src/argparse.{cpp,h} stays listed here — see the
# note by VENDOR_TEXTEDITOR_SOURCES below, it is NOT actually a third-party
# library despite the name.
VENDORED_EXCLUDE := src/argparse.cpp src/argparse.h src/compat/% src/m4board_web_assets.h
FORMAT_SOURCES := $(filter-out $(VENDORED_EXCLUDE),$(SOURCES))
FORMAT_HEADERS := $(filter-out $(VENDORED_EXCLUDE),$(HEADERS))

# Vendored ImGuiColorTextEdit .cpp sources (relocated out of src/, so the
# `find $(SRCDIR)` glob above no longer picks them up — add them back
# explicitly, gated the same way MODERN_UI_FILES used to gate them in place).
VENDOR_TEXTEDITOR_SOURCES := vendor/ImGuiColorTextEdit/TextEditor.cpp vendor/ImGuiColorTextEdit/LanguageDefinitions.cpp

# Modern UI gate (P1.5.1 step 4).  KONCPC_MODERN_UI=1 (default) builds the
# Dear ImGui + SDL_GPU UI as today.  =0 excludes UI-only sources so the
# core can compile without the modern UI — used by the future headless
# build (P1.5.2).  Mirror in CMakeLists.txt (KONCPC_BUILD_MODERN_UI).
KONCPC_MODERN_UI ?= 1
# Keep this list in sync with MODERN_UI_FILES in CMakeLists.txt.
MODERN_UI_FILES := imgui_ui imgui_ui_host devtools_ui command_palette workspace_layout video
MODERN_UI_SOURCES := $(addprefix $(SRCDIR)/,$(addsuffix .cpp,$(MODERN_UI_FILES)))
ifeq ($(KONCPC_MODERN_UI),1)
COMMON_CFLAGS += -DKONCPC_MODERN_UI
IMGUI_SOURCES:=vendor/imgui/imgui.cpp vendor/imgui/imgui_draw.cpp vendor/imgui/imgui_tables.cpp vendor/imgui/imgui_widgets.cpp vendor/imgui/backends/imgui_impl_sdl3.cpp vendor/imgui/backends/imgui_impl_sdlrenderer3.cpp vendor/imgui/backends/imgui_impl_sdlgpu3.cpp
SOURCES += $(VENDOR_TEXTEDITOR_SOURCES)
# Their objects are built by a dedicated relaxed-warnings rule (like ImGui), so
# keep them out of the generic $(OBJECTS_CPP) rule to avoid a double recipe.
VENDORED_TEXTEDITOR := $(addprefix $(OBJDIR)/,$(VENDOR_TEXTEDITOR_SOURCES:.cpp=.o))
else
$(info KONCPC_MODERN_UI=0 — excluding modern UI sources.  Full link will fail until P1.5.2 lands a headless main and an ImGui-free imgui_ui.h split.)
SOURCES := $(filter-out $(MODERN_UI_SOURCES),$(SOURCES))
IMGUI_SOURCES :=
endif

DEPENDS:=$(foreach file,$(SOURCES:.cpp=.d),$(shell echo "$(OBJDIR)/$(file)"))
MM_DEPENDS:=$(foreach file,$(MM_SOURCES:.mm=.d),$(shell echo "$(OBJDIR)/$(file)"))
OBJECTS_CPP:=$(filter-out $(VENDORED_TEXTEDITOR),$(DEPENDS:.d=.o))
OBJECTS_MM:=$(MM_DEPENDS:.d=.o)
IMGUI_OBJECTS:=$(foreach file,$(IMGUI_SOURCES:.cpp=.o),$(OBJDIR)/$(file))

OBJECTS:=$(OBJECTS_CPP) $(OBJECTS_MM) $(IMGUI_OBJECTS) $(VENDORED_TEXTEDITOR)

TEST_SOURCES:=$(shell find $(TSTDIR) -name \*.cpp)
TEST_HEADERS:=$(shell find $(TSTDIR) -name \*.h)
TEST_DEPENDS:=$(foreach file,$(TEST_SOURCES:.cpp=.d),$(shell echo "$(OBJDIR)/$(file)"))
TEST_OBJECTS:=$(TEST_DEPENDS:.d=.o)

.PHONY: all check_deps clean deb_pkg debug debug_flag distrib doc tags unit_test install doxygen coverage coverage-report coverage-clean sim sim_headless bench pgo

WARNINGS = -Wall -Wextra -Wzero-as-null-pointer-constant -Wformat=2 -Wold-style-cast -Wmissing-include-dirs -Woverloaded-virtual -Wpointer-arith -Wredundant-decls -Wimplicit-fallthrough
# Tier 1: always-errors even in release (undefined behavior / security critical)
WARN_AS_ERRORS = -Werror=return-type -Werror=format-security -Werror=implicit-fallthrough -Werror=uninitialized -Werror=array-bounds
COMMON_CFLAGS += -std=c++17 $(IPATHS)
DEBUG_FLAGS = -Werror -g -O0 -DDEBUG
RELEASE_FLAGS = -O2 -funroll-loops -ffast-math -fomit-frame-pointer -finline-functions
# Whole-program LTO: measured +6% GUI / +2% headless throughput on the subcycle
# engine and ~24% smaller binary (2026-07-04, beads-fc0b); full suite green under
# it. macOS/clang only for now — MINGW/Linux CI LTO is unverified (tracked
# separately). Release builds only (debug keeps fast, non-LTO links).
ifeq ($(ARCH),macos)
RELEASE_FLAGS += -flto
LDFLAGS += -flto
endif
# Opt-in "soldered board" fast path (see docs/board-performance.md). Replaces the
# pluggable fn-pointer array dispatch with a fixed, inlinable direct-call sequence
# (machine.cpp tick_soldered) — measured ~1.5x throughput under LTO. It TRADES
# AWAY per-device (and cross-language) replaceability and runtime composition, so
# it is OFF by default: the pluggable Device contract is the shipping path.
# Enable with `make SOLDERED=1`.
ifdef SOLDERED
RELEASE_FLAGS += -DSOLDERED
endif
# PGO (profile-guided optimization) for the GUI binary — manual 2-phase flow
# (the `make pgo` target below covers the headless bench only).
# Phase 1 (instrument): make clean && make PGO_GEN=1
#   then TRAIN ON THE SHIPPING ENGINE — the sub-cycle wake tier — on both
#   workloads (plus a short legacy run while engine=0 still exists), headless
#   and uncapped, with a continuous-mode profile file (the %c mmaps the
#   counters and flushes live — REQUIRED here because cleanExit() calls _exit(),
#   which skips LLVM's atexit profile writer):
#   LLVM_PROFILE_FILE="kon-%p-%c.profraw" KONCPC_WAKE=0x3FF \
#     SDL_VIDEODRIVER=dummy ./koncepcja --fps -O system.limit_speed=0 \
#     -O system.engine=1 [-O system.model=3 rom/system.cpr]
#   (Measured 2026-07-09: engine=1-trained PGO buys the sub-cycle GUI +34%/+37%
#   on 6128/Plus and the legacy path keeps its gains from one combined profile;
#   a legacy-only profile does NOT help engine=1.)
# Phase 2 (use): xcrun llvm-profdata merge -o kon.profdata kon-*.profraw
#   then  make clean && make PGO_USE=$PWD/kon.profdata
ifdef PGO_GEN
RELEASE_FLAGS += -fprofile-instr-generate
LDFLAGS += -fprofile-instr-generate
endif
ifdef PGO_USE
RELEASE_FLAGS += -fprofile-instr-use=$(PGO_USE) -Wno-error=profile-instr-out-of-date -Wno-error=profile-instr-unprofiled
LDFLAGS += -fprofile-instr-use=$(PGO_USE)
endif
# Strip symbols in release builds (linker flag; skipped on macOS where it's unsupported by clang)
ifneq ($(ARCH),macos)
RELEASE_FLAGS += -s
endif

ifeq ($(findstring "g++",$(CXX)),"g++")
WARNINGS += -Wlogical-op
RELEASE_FLAGS += -fno-strength-reduce
endif

BUILD_FLAGS = $(RELEASE_FLAGS)

debug: BUILD_FLAGS:=$(DEBUG_FLAGS)

ifndef DEBUG
ifeq ($(LAST_BUILD_IN_DEBUG), 1)
FORCED_DEBUG = 1
DEBUG = 1
endif
endif


ifdef DEBUG
BUILD_FLAGS = $(DEBUG_FLAGS)
all: check_deps debug
else
all: check_deps distrib
endif

# gtest doesn't build with warnings flags, hence the COMMON_CFLAGS
# WARN_SUPPRESS and CFLAGS come last so platform defaults and user overrides
# can disable specific warnings triggered by vendor code
ALL_CFLAGS=$(COMMON_CFLAGS) $(WARNINGS) $(WARN_AS_ERRORS) $(WARN_SUPPRESS) $(CFLAGS)

####################################
### Coverage support
####################################
ifdef COVERAGE
ifeq ($(UNAME_S),Darwin)
# LLVM source-based coverage for macOS (clang)
COVERAGE_FLAGS = -fprofile-instr-generate -fcoverage-mapping
else
# gcov-style coverage for Linux (gcc) - compatible with codecov/coveralls
COVERAGE_FLAGS = --coverage -fprofile-arcs -ftest-coverage
endif
ALL_CFLAGS += $(COVERAGE_FLAGS)
LDFLAGS += $(COVERAGE_FLAGS)
endif

$(MAIN): main.cpp src/koncepcja.h
	@$(CXX) -c $(BUILD_FLAGS) $(ALL_CFLAGS) -o $(MAIN) main.cpp

$(DEPENDS): $(OBJDIR)/%.d: %.cpp
	@echo Computing dependencies for $<
	@mkdir -p `dirname $@`
	@$(CXX) -MM $(BUILD_FLAGS) $(ALL_CFLAGS) $< | { sed 's#^[^:]*\.o[ :]*#$(OBJDIR)/$*.o $(OBJDIR)/$*.os $(OBJDIR)/$*.d : #g' ; echo "%.h:;" ; echo "" ; } > $@

$(OBJECTS_CPP): $(OBJDIR)/%.o: %.cpp
	@mkdir -p `dirname $@`
	$(CXX) -c $(BUILD_FLAGS) $(ALL_CFLAGS) -o $@ $<

$(MM_DEPENDS): $(OBJDIR)/%.d: %.mm
	@echo Computing dependencies for $<
	@mkdir -p `dirname $@`
	@$(CXX) -MM $(BUILD_FLAGS) $(ALL_CFLAGS) $< | { sed 's#^[^:]*\.o[ :]*#$(OBJDIR)/$*.o $(OBJDIR)/$*.os $(OBJDIR)/$*.d : #g' ; echo "%.h:;" ; echo "" ; } > $@

$(MM_DEPENDS:.d=.o): $(OBJDIR)/%.o: %.mm
	@mkdir -p `dirname $@`
	$(CXX) -c $(BUILD_FLAGS) $(ALL_CFLAGS) -o $@ $<

# Vendored TextEditor: compile with relaxed warnings (like ImGui).
# ($(VENDORED_TEXTEDITOR) is defined up with the source list, and filtered out
# of $(OBJECTS_CPP) so only this rule builds it.)
$(VENDORED_TEXTEDITOR): $(OBJDIR)/%.o: %.cpp
	@mkdir -p `dirname $@`
	$(CXX) -c $(BUILD_FLAGS) $(COMMON_CFLAGS) -o $@ $<

$(IMGUI_OBJECTS): $(OBJDIR)/%.o: %.cpp
	@mkdir -p `dirname $@`
	$(CXX) -c $(BUILD_FLAGS) $(COMMON_CFLAGS) -o $@ $<

debug: debug_flag tags distrib unit_test

debug_flag:
ifdef FORCED_DEBUG
	@echo -e '\n!!!!!!!!!!!\n!! Warning: previous build was in debug - rebuilding in debug.\n!! Use make clean before running make to rebuild in release.\n!!!!!!!!!!!\n'
endif
	@touch .debug

ifeq ($(PLATFORM),linux)
check_deps:
	@pkg-config --cflags sdl3 >/dev/null 2>&1 || (echo "Error: missing dependency SDL3. Try installing libsdl3 development package" && false)
	@pkg-config --version >/dev/null 2>&1 || (echo "Error: missing pkg-config. Try installing pkg-config" && false)
	@pkg-config --cflags freetype2 >/dev/null 2>&1 || (echo "Error: missing dependency libfreetype. Try installing libfreetype development package (e.g: libfreetype6-dev)" && false)
	@pkg-config --cflags zlib >/dev/null 2>&1 || (echo "Error: missing dependency zlib. Try installing zlib development package (e.g: zlib1g-dev)" && false)
	@pkg-config --cflags libpng >/dev/null 2>&1 || (echo "Error: missing dependency libpng. Try installing libpng development package (e.g: libpng-dev)" && false)
else
# TODO(cpitrat): Implement check_deps for windows build
check_deps:
endif

tags:
	@ctags -R main.cpp src || echo -e "!!!!!!!!!!!\n!! Warning: ctags not found - if you are a developer, you might want to install it.\n!!!!!!!!!!!"

doc: $(HTML_DOC)

$(HTML_DOC): $(GROFF_DOC)
	groff -mandoc -Thtml $< > $@

koncepcja.cfg: koncepcja.cfg.tmpl
	@sed 's/__SHARE_PATH__.*//' koncepcja.cfg.tmpl > koncepcja.cfg

$(TARGET): $(OBJECTS) $(MAIN) koncepcja.cfg
	$(CXX) $(LDFLAGS) -o $(TARGET) $(OBJECTS) $(MAIN) $(LIBS)
ifeq ($(ARCH),macos)
	$(call SIGN_MACOS,$(TARGET),)
endif

ifeq ($(PLATFORM),windows)
DLLS = SDL3.dll libbz2-1.dll libfreetype-6.dll libpng16-16.dll libstdc++-6.dll \
       libwinpthread-1.dll zlib1.dll libglib-2.0-0.dll libgraphite2.dll \
       libharfbuzz-0.dll libiconv-2.dll libintl-8.dll libpcre2-8-0.dll \
			 libbrotlidec.dll libbrotlicommon.dll

distrib: $(TARGET)
	mkdir -p $(ARCHIVE_DIR)
	rm -f $(RELEASE_DIR)/$(ARCHIVE).zip
	cp $(TARGET) $(ARCHIVE_DIR)/
	$(foreach DLL,$(DLLS),[ -f $(MINGW_PATH)/bin/$(DLL) ] && cp $(MINGW_PATH)/bin/$(DLL) $(ARCHIVE_DIR)/ || (echo "$(MINGW_PATH)/bin/$(DLL) doesn't exist" && false);)
	cp $(MINGW_PATH)/bin/libgcc_s_*-1.dll $(ARCHIVE_DIR)/
	cp koncepcja.cfg.tmpl koncepcja.cfg COPYING.txt README.md $(ARCHIVE_DIR)/
	cp -r resources/ rom/ licenses/ $(ARCHIVE_DIR)/
	cd $(RELEASE_DIR) && zip -r $(ARCHIVE).zip $(ARCHIVE)

install: $(TARGET)

else

ifeq ($(ARCH),macos)

# Create a zip with a koncepcja binary that should work launched locally
distrib: $(TARGET)
	mkdir -p $(ARCHIVE_DIR)
	rm -f $(RELEASE_DIR)/$(ARCHIVE).zip
	cp $(TARGET) $(ARCHIVE_DIR)/
	cp -r rom resources doc licenses $(ARCHIVE_DIR)
	cp koncepcja.cfg README.md COPYING.txt $(ARCHIVE_DIR)
	cd $(RELEASE_DIR) && zip -r $(ARCHIVE).zip $(ARCHIVE)

else

SRC_PACKAGE_DIR=$(ARCHIVE_DIR)/koncepcja-$(VERSION)

# Create a debian source package
distrib: $(TARGET)
	mkdir -p $(SRC_PACKAGE_DIR)
	rm -fr $(SRC_PACKAGE_DIR)/*
	cp -r src rom resources doc licenses debian $(SRC_PACKAGE_DIR)
	cp main.cpp koncepcja.cfg.tmpl koncepcja.cfg makefile README.md INSTALL.md COPYING.txt $(SRC_PACKAGE_DIR)
	tar jcf $(SRC_PACKAGE_DIR).tar.bz2 -C $(ARCHIVE_DIR) koncepcja-$(VERSION)
	ln -s koncepcja-$(VERSION).tar.bz2 $(ARCHIVE_DIR)/koncepcja_$(VERSION).orig.tar.bz2 || true

endif  # ARCH =? macos

install: $(TARGET)
	install -D $(TARGET) $(DESTDIR)$(prefix)/bin/$(TARGET)
	install -D $(GROFF_DOC) $(DESTDIR)$(prefix)/share/man/man6/koncepcja.6
	if [ ! -f $(DESTDIR)/etc/koncepcja.cfg ]; then \
		install -D -m664 koncepcja.cfg.tmpl $(DESTDIR)/etc/koncepcja.cfg; \
		sed -i "s,__SHARE_PATH__,$(DESTDIR)$(prefix)/share/koncepcja," $(DESTDIR)/etc/koncepcja.cfg; \
	fi
	mkdir -p $(DESTDIR)$(prefix)/share/koncepcja
	cp -r resources rom $(DESTDIR)$(prefix)/share/koncepcja

endif

####################################
### Tests
####################################

googletest:
	@[ -d googletest ] || git clone https://github.com/google/googletest.git

TEST_CFLAGS = $(COMMON_CFLAGS) -I$(GTEST_DIR)/include -I$(GTEST_DIR) -I$(GMOCK_DIR)/include -I$(GMOCK_DIR)
GTEST_DIR = googletest/googletest/
GMOCK_DIR = googletest/googlemock/

$(GTEST_DIR)/src/gtest-all.cc: googletest
$(GMOCK_DIR)/src/gmock-all.cc: googletest

$(TEST_DEPENDS): $(OBJDIR)/%.d: %.cpp googletest
	@echo Computing dependencies for $<
	@mkdir -p `dirname $@`
	@$(CXX) -MM $(BUILD_FLAGS) $(TEST_CFLAGS) $< | { sed 's#^[^:]*\.o[ :]*#$(OBJDIR)/$*.o $(OBJDIR)/$*.d : #g' ; echo "%.h:;" ; echo "" ; } > $@

$(TEST_OBJECTS): $(OBJDIR)/%.o: %.cpp googletest
	$(CXX) -c $(BUILD_FLAGS) $(TEST_CFLAGS) -o $@ $<

$(OBJDIR)/$(GTEST_DIR)/src/gtest-all.o: $(GTEST_DIR)/src/gtest-all.cc googletest
	@mkdir -p `dirname $@`
	$(CXX) -c $(BUILD_FLAGS) $(TEST_CFLAGS) -o $@ $<

$(OBJDIR)/$(GMOCK_DIR)/src/gmock-all.o: $(GMOCK_DIR)/src/gmock-all.cc googletest
	@mkdir -p `dirname $@`
	$(CXX) -c $(BUILD_FLAGS) $(TEST_CFLAGS) -o $@ $<

$(TEST_TARGET): $(OBJECTS) $(TEST_OBJECTS) $(OBJDIR)/$(GTEST_DIR)/src/gtest-all.o $(OBJDIR)/$(GMOCK_DIR)/src/gmock-all.o
	$(CXX) $(LDFLAGS) -o $(TEST_TARGET) $(OBJDIR)/$(GTEST_DIR)/src/gtest-all.o $(OBJDIR)/$(GMOCK_DIR)/src/gmock-all.o $(TEST_OBJECTS) $(OBJECTS) $(LIBS) -lpthread

# --- Standalone chip-simulation host --------------------------------------------
# Runs the src/hw simulation cores (Z80/GA/CRTC/PPI/PSG/memory/video) as a live CPC in
# an SDL window. Self-contained: only the hw Devices + SDL, NOT the legacy loop, so the
# per-chip simulations stay swappable and independently verifiable.
SIM_TARGET = koncepcja_sim
SIM_HW_SRCS = $(wildcard src/hw/*.cpp) $(wildcard src/subcycle/*.cpp)
sim: sim/koncepcja_sim.cpp $(SIM_HW_SRCS)
	$(CXX) -std=c++17 -O2 -Isrc $(PKG_SDL_CFLAGS) -o $(SIM_TARGET) $^ $(PKG_SDL_LIBS) $(LDFLAGS)

# Headless build (no SDL): batch --frames/--shot only, for CI / smoke tests.
sim_headless: sim/koncepcja_sim.cpp $(SIM_HW_SRCS)
	$(CXX) -std=c++17 -O2 -Isrc -DSIM_HEADLESS_ONLY -o $(SIM_TARGET)_headless $^

# --- FPS benchmark + PGO 2-phase flow (beads-lcfa / plan §10-B4, risk #5) -------
# A FIXED, deterministic headless cold-boot trace (sim/bench_fps.cpp) reusing the
# sim source set (hw Devices + subcycle) — no SDL, no legacy loop — so the FPS is
# the engine that actually ships. It is both the PGO training workload AND the
# thing measured. Dispatch path is a BUILD property, not a runtime flag:
#   make bench          → pluggable board_tick (the shipping DEFAULT path)
#   make bench SOLDERED=1→ tick_soldered (measurement-only direct dispatch)
# `make pgo` runs the full instrument -> train -> merge -> optimise flow and
# prints the FPS; add SOLDERED=1 for the monomorphic ceiling that quoted "114".
#
# LTO/PGO here are clang/llvm on macos+linux ONLY and are NEVER applied to the
# MINGW toolchain: the pgo target hard-errors for win32/win64, and BENCH_OPT is
# expanded only inside these opt-in recipes, never in the default/distrib build.
BENCH_TARGET = koncepcja_bench
BENCH_SRCS = sim/bench_fps.cpp $(SIM_HW_SRCS)
# Release-tier optimisation, mirroring RELEASE_FLAGS' core (LTO on macos as in the
# shipping build). SOLDERED=1 compiles the direct-dispatch measurement path.
BENCH_OPT = -O2 -funroll-loops -ffast-math -fomit-frame-pointer -finline-functions
ifeq ($(ARCH),macos)
BENCH_OPT += -flto
endif
ifdef SOLDERED
BENCH_OPT += -DSOLDERED
endif

bench: $(BENCH_SRCS)
	$(CXX) -std=c++17 $(BENCH_OPT) -Isrc -o $(BENCH_TARGET) $^
	./$(BENCH_TARGET) --frames $(PGO_BENCH_FRAMES)

# PGO artefacts (git-ignored; regenerated by `make pgo`) and trace lengths.
PGO_PROFRAW = $(BENCH_TARGET).profraw
PGO_PROFDATA = $(BENCH_TARGET).profdata
PGO_TRAIN_FRAMES ?= 1500
PGO_BENCH_FRAMES ?= 2000
# Train and measure the SHIPPING dispatch: the wake tier with every predicate
# armed (Gate B6; 0x3FF = all ten bits, see machine.cpp KONCPC_WAKE). PGO_WAKE=0
# keeps the wake dispatcher but forces every device awake per cycle (the bisect
# baseline).
PGO_WAKE ?= 0x3FF

ifeq ($(filter $(ARCH),macos linux),)
pgo:
	@echo "make pgo: profile-guided build is clang/llvm (macos/linux) only; ARCH=$(ARCH) is unsupported." && false
else
ifeq ($(ARCH),macos)
LLVM_PROFDATA ?= xcrun llvm-profdata
else
LLVM_PROFDATA ?= llvm-profdata
endif
# bench returns from main normally (no _exit), so LLVM's atexit writer flushes
# the counters — a plain profraw name suffices (the koncepcja binary needs %c
# only because cleanExit() calls _exit(); see the RELEASE_FLAGS note above).
pgo: $(BENCH_SRCS)
	@echo "==> PGO 1/3: instrument ($(if $(SOLDERED),soldered,pluggable) dispatch)"
	$(CXX) -std=c++17 $(BENCH_OPT) -fprofile-instr-generate -Isrc -o $(BENCH_TARGET)_gen $(BENCH_SRCS)
	@echo "==> PGO 2/3: train on the fixed headless traces ($(PGO_TRAIN_FRAMES) frames, 6128 + Plus, wake + fast tiers)"
	LLVM_PROFILE_FILE="$(PGO_PROFRAW)" KONCPC_WAKE=$(PGO_WAKE) ./$(BENCH_TARGET)_gen --frames $(PGO_TRAIN_FRAMES) --quiet
	LLVM_PROFILE_FILE="$(PGO_PROFRAW).plus" KONCPC_WAKE=$(PGO_WAKE) ./$(BENCH_TARGET)_gen --cpr rom/system.cpr --frames $(PGO_TRAIN_FRAMES) --quiet
	LLVM_PROFILE_FILE="$(PGO_PROFRAW).fast" KONCPC_TIER=fast ./$(BENCH_TARGET)_gen --frames $(PGO_TRAIN_FRAMES) --quiet
	LLVM_PROFILE_FILE="$(PGO_PROFRAW).fastplus" KONCPC_TIER=fast ./$(BENCH_TARGET)_gen --cpr rom/system.cpr --frames $(PGO_TRAIN_FRAMES) --quiet
	$(LLVM_PROFDATA) merge -o $(PGO_PROFDATA) $(PGO_PROFRAW) $(PGO_PROFRAW).plus $(PGO_PROFRAW).fast $(PGO_PROFRAW).fastplus
	@echo "==> PGO 3/3: rebuild optimised with the profile, then measure"
	$(CXX) -std=c++17 $(BENCH_OPT) -fprofile-instr-use=$(PGO_PROFDATA) -Wno-error=profile-instr-out-of-date -Wno-error=profile-instr-unprofiled -Isrc -o $(BENCH_TARGET) $(BENCH_SRCS)
	KONCPC_WAKE=$(PGO_WAKE) ./$(BENCH_TARGET) --frames $(PGO_BENCH_FRAMES)
endif

ifeq ($(PLATFORM),windows)
unit_test: $(TEST_TARGET) distrib
	cp $(TEST_TARGET) $(ARCHIVE_DIR)/
	rm -fr $(ARCHIVE_DIR)/test
	ln -s -f ../../test $(ARCHIVE_DIR)/test
	cd $(ARCHIVE_DIR) && ./$(TEST_TARGET) --gtest_shuffle

e2e_test: $(TARGET)
	cd test/integrated && ./run_tests.sh
else
unit_test: $(TEST_TARGET)
	./$(TEST_TARGET) --gtest_shuffle

e2e_test: $(TARGET)
	cd test/integrated && ./run_tests.sh
endif

deb_pkg: all
	# Both changelog files need to be patched with the proper version !
	sed -i "1s/(.*)/($(VERSION)-$(REVISION))/" debian/changelog
	sed -i "1s/(.*)/($(VERSION)-$(REVISION))/" $(ARCHIVE_DIR)/koncepcja-$(VERSION)/debian/changelog
	cd $(ARCHIVE_DIR)/koncepcja-$(VERSION)/debian && debuild -e CXX -us -uc --lintian-opts --profile debian

BUNDLE_DIR=release/koncepcja-macos-bundle/konCePCja.app
macos_bundle: all
	rm -rf $(BUNDLE_DIR)
	mkdir -p $(BUNDLE_DIR)/Contents/MacOS
	mkdir -p $(BUNDLE_DIR)/Contents/Resources
	install $(TARGET) $(BUNDLE_DIR)/Contents/MacOS/$(TARGET)
	install resources/Info.plist $(BUNDLE_DIR)/Contents/
	install -m664 koncepcja.cfg.tmpl $(BUNDLE_DIR)/Contents/Resources/koncepcja.cfg
	gsed -i "s,__SHARE_PATH__,../Resources," $(BUNDLE_DIR)/Contents/Resources/koncepcja.cfg
	cp -r resources rom $(BUNDLE_DIR)/Contents/Resources
	mkdir -p $(BUNDLE_DIR)/Contents/Frameworks
	# Copy shared libs — skip @rpath entries (handled separately below)
	for lib in $$(otool -L $(BUNDLE_DIR)/Contents/MacOS/$(TARGET) | grep ".dylib" | awk '{ print $$1 }' | grep -v @); do \
		echo "Copying '$$lib'"; cp "$$lib" $(BUNDLE_DIR)/Contents/Frameworks/; \
	done || true
	# Copy SDL3 from vendored install (otool shows @rpath, not the actual path)
	cp vendor/SDL/install/lib/libSDL3.0.dylib $(BUNDLE_DIR)/Contents/Frameworks/
	# Rewrite all dylib paths to use @executable_path so the bundle is self-contained
	for lib in $(BUNDLE_DIR)/Contents/Frameworks/*.dylib; do \
		libname=$$(basename "$$lib"); \
		install_name_tool -change "$$(otool -L $(BUNDLE_DIR)/Contents/MacOS/$(TARGET) | grep "$$libname" | awk '{ print $$1 }')" "@executable_path/../Frameworks/$$libname" $(BUNDLE_DIR)/Contents/MacOS/$(TARGET); \
	done
	# Sign the bundle if Developer ID cert is available, otherwise ad-hoc
	$(call SIGN_MACOS,$(BUNDLE_DIR),--deep)
	# Retry hdiutil up to 3 times: it occasionally fails with "Resource Busy"
	for i in 1 2 3; do hdiutil create -volname konCePCja-$(VERSION) -srcfolder $(BUNDLE_DIR) -ov -format UDZO release/koncepcja-macos-bundle/konCePCja.dmg && break || sleep 5; done

clang-tidy:
	if $(CLANG_TIDY) $(FORMAT_SOURCES) -header-filter=src/* -- $(COMMON_CFLAGS) | grep "."; then false; fi
	./tools/check_includes.sh

clang-format:
	./tools/check_clang_format.sh $(CLANG_FORMAT) "-style=Google" $(FORMAT_SOURCES) $(TEST_SOURCES) $(FORMAT_HEADERS) $(TEST_HEADERS)

fix-clang-format:
	$(CLANG_FORMAT) -style=Google -i $(FORMAT_SOURCES) $(TEST_SOURCES) $(FORMAT_HEADERS) $(TEST_HEADERS)

doxygen:
	doxygen doxygen.cfg

####################################
### Coverage targets
####################################

coverage: clean
	$(MAKE) COVERAGE=1 unit_test
	$(MAKE) coverage-report

# Prefer xcrun-wrapped LLVM tools (Xcode), fall back to bare commands
ifeq ($(UNAME_S),Darwin)
  LLVM_PROFDATA := $(shell command -v xcrun >/dev/null 2>&1 && echo "xcrun llvm-profdata" || echo "llvm-profdata")
  LLVM_COV := $(shell command -v xcrun >/dev/null 2>&1 && echo "xcrun llvm-cov" || echo "llvm-cov")
endif

coverage-report:
ifeq ($(UNAME_S),Darwin)
	@mkdir -p coverage
	LLVM_PROFILE_FILE="coverage-%p.profraw" ./$(TEST_TARGET) --gtest_shuffle || true
	$(LLVM_PROFDATA) merge -sparse coverage-*.profraw -o coverage.profdata
	$(LLVM_COV) show ./$(TEST_TARGET) -instr-profile=coverage.profdata -format=html -output-dir=coverage/html src/
	$(LLVM_COV) report ./$(TEST_TARGET) -instr-profile=coverage.profdata src/
	@echo "Coverage report: coverage/html/index.html"
else
	@mkdir -p coverage
	lcov --directory obj/ --capture --output-file coverage/coverage.info \
		--exclude '/usr/include/*' \
		--exclude '*/googletest/*' \
		--exclude '*/vendor/*' \
		--exclude '*/test/*' || true
	genhtml coverage/coverage.info --output-directory coverage/html --demangle-cpp || true
	gcovr --root . --filter='src/.*' --exclude='vendor/.*' --exclude='googletest/.*' --exclude='test/.*' \
		--html coverage/gcovr.html --json coverage/coverage.json --print-summary || true
	@echo "Coverage reports:"
	@echo "  HTML: coverage/html/index.html"
	@echo "  GCovr: coverage/gcovr.html"
	@echo "  JSON: coverage/coverage.json"
endif

coverage-clean:
	rm -rf coverage/ coverage-*.profraw coverage.profdata coverage.info
	find obj/ -name "*.gcda" -exec rm -f {} + 2>/dev/null || true
	find obj/ -name "*.gcno" -exec rm -f {} + 2>/dev/null || true

clean:
	rm -rf obj/ release/ .pc/ doxygen/
	rm -f test_runner test_runner.exe koncepcja koncepcja.exe .debug tags
	rm -f koncepcja_sim koncepcja_sim_headless koncepcja_bench koncepcja_bench_gen
	rm -f koncepcja_bench.profraw koncepcja_bench.profdata

-include $(DEPENDS) $(TEST_DEPENDS)
