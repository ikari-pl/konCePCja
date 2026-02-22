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
COMMON_CFLAGS += -DGL_SILENCE_DEPRECATION
WARN_SUPPRESS = -Wno-error=old-style-cast -Wno-error=zero-as-null-pointer-constant -Wno-error=missing-braces -Wno-error=deprecated-declarations -Wno-error=self-assign -Wno-error=vla-cxx-extension
LDFLAGS += -framework Cocoa -framework OpenGL
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

CAPS_INCLUDES=-Isrc/capsimg/LibIPF -Isrc/capsimg/Device -Isrc/capsimg/CAPSImg -Isrc/capsimg/Codec -Isrc/capsimg/Core

PKG_SDL_CFLAGS=`pkg-config --cflags sdl3`
PKG_SDL_LIBS=`pkg-config --libs sdl3`
# SDL_image optional support removed; PNG loads via libpng
ifeq ($(ARCH),macos)
ifeq ($(USE_VENDORED_SDL),1)
PKG_SDL_CFLAGS=$(SDL_VENDOR_INCLUDE)
PKG_SDL_LIBS=$(SDL_VENDOR_LIBS)
ifeq ($(ARCH),macos)
LDFLAGS += -Wl,-rpath,@executable_path/$(SDL_VENDOR_BUILD)/lib
endif
endif
endif
IPATHS = -Isrc/ $(CAPS_INCLUDES) -Ivendor/imgui -Ivendor/imgui/backends `pkg-config --cflags freetype2` $(PKG_SDL_CFLAGS) `pkg-config --cflags libpng` `pkg-config --cflags zlib`
LIBS = $(PKG_SDL_LIBS) `pkg-config --libs freetype2` `pkg-config --libs libpng` `pkg-config --libs zlib`
ifeq ($(PLATFORM),windows)
LIBS += -lws2_32 -lopengl32
else ifeq ($(ARCH),linux)
LIBS += -lGL
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
DEPENDS:=$(foreach file,$(SOURCES:.cpp=.d),$(shell echo "$(OBJDIR)/$(file)"))
MM_DEPENDS:=$(foreach file,$(MM_SOURCES:.mm=.d),$(shell echo "$(OBJDIR)/$(file)"))
OBJECTS_CPP:=$(DEPENDS:.d=.o)
OBJECTS_MM:=$(MM_DEPENDS:.d=.o)
IMGUI_SOURCES:=vendor/imgui/imgui.cpp vendor/imgui/imgui_draw.cpp vendor/imgui/imgui_tables.cpp vendor/imgui/imgui_widgets.cpp vendor/imgui/backends/imgui_impl_sdl3.cpp vendor/imgui/backends/imgui_impl_opengl3.cpp
IMGUI_OBJECTS:=$(foreach file,$(IMGUI_SOURCES:.cpp=.o),$(OBJDIR)/$(file))

OBJECTS:=$(OBJECTS_CPP) $(OBJECTS_MM) $(IMGUI_OBJECTS)

TEST_SOURCES:=$(shell find $(TSTDIR) -name \*.cpp)
TEST_HEADERS:=$(shell find $(TSTDIR) -name \*.h)
TEST_DEPENDS:=$(foreach file,$(TEST_SOURCES:.cpp=.d),$(shell echo "$(OBJDIR)/$(file)"))
TEST_OBJECTS:=$(TEST_DEPENDS:.d=.o)

.PHONY: all check_deps clean deb_pkg debug debug_flag distrib doc tags unit_test install doxygen coverage coverage-report coverage-clean

WARNINGS = -Wall -Wextra -Wzero-as-null-pointer-constant -Wformat=2 -Wold-style-cast -Wmissing-include-dirs -Woverloaded-virtual -Wpointer-arith -Wredundant-decls
COMMON_CFLAGS += -std=c++17 $(IPATHS)
DEBUG_FLAGS = -Werror -g -O0 -DDEBUG
RELEASE_FLAGS = -O2 -funroll-loops -ffast-math -fomit-frame-pointer -finline-functions
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

ifndef WITHOUT_GL
COMMON_CFLAGS += -DWITH_GL
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
ALL_CFLAGS=$(COMMON_CFLAGS) $(WARNINGS) $(WARN_SUPPRESS) $(CFLAGS)

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

$(TEST_DEPENDS): $(OBJDIR)/%.d: %.cpp
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
	if $(CLANG_TIDY) -checks=-*,$(CLANG_CHECKS) $(SOURCES) -header-filter=src/* -- $(COMMON_CFLAGS) | grep "."; then false; fi
	./tools/check_includes.sh

clang-format:
	./tools/check_clang_format.sh $(CLANG_FORMAT) "-style=Google" $(SOURCES) $(TEST_SOURCES) $(HEADERS) $(TEST_HEADERS)

fix-clang-format:
	$(CLANG_FORMAT) -style=Google -i $(SOURCES) $(TEST_SOURCES) $(HEADERS) $(TEST_HEADERS)

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

-include $(DEPENDS) $(TEST_DEPENDS)
