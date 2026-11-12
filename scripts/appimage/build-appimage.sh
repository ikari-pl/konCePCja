#!/bin/bash
# Builds release/linux-packages/konCePCja-<version>-<arch>.AppImage from a
# checkout with submodules. Run it through `make appimage`, which passes
# VERSION (from .release-please-manifest.json).
#
# What goes in: the koncepcja binary, the vendored SDL3 it was linked
# against, every other library it links that is not on the AppImage
# exclude list (linuxdeploy applies that list: glibc, libGL, the X11 client
# libraries and friends stay the host's), share/koncepcja/{resources,rom},
# the desktop file and icons. Build it on the oldest distribution it should
# run on: the bundled libraries and the glibc symbols the binary asks for
# come from the build machine.
#
# The tools are pinned by version and SHA-256 and downloaded into
# $APPIMAGE_TOOLS_DIR (default release/appimage/tools). No FUSE is needed:
# the tools run with APPIMAGE_EXTRACT_AND_RUN=1.
set -euo pipefail

: "${VERSION:?VERSION must be set (make appimage sets it)}"
cd "$(dirname "$0")/../.."

ARCH=$(uname -m)
case "$ARCH" in
  x86_64)
    LINUXDEPLOY_SHA=c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
    APPIMAGETOOL_SHA=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
    RUNTIME_SHA=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d
    ;;
  aarch64)
    LINUXDEPLOY_SHA=620095110d693282b8ebeb244a95b5e911cf8f65f76c88b4b47d16ae6346fcff
    APPIMAGETOOL_SHA=f0837e7448a0c1e4e650a93bb3e85802546e60654ef287576f46c71c126a9158
    RUNTIME_SHA=00cbdfcf917cc6c0ff6d3347d59e0ca1f7f45a6df1a428a0d6d8a78664d87444
    ;;
  *)
    echo "build-appimage.sh: no pinned tools for architecture $ARCH" >&2
    exit 1
    ;;
esac
LINUXDEPLOY_URL=https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-$ARCH.AppImage
APPIMAGETOOL_URL=https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-$ARCH.AppImage
RUNTIME_URL=https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-$ARCH

TOOLS=${APPIMAGE_TOOLS_DIR:-release/appimage/tools}
APPDIR=release/appimage/AppDir
OUT_DIR=release/linux-packages
OUT=$OUT_DIR/konCePCja-$VERSION-$ARCH.AppImage
JOBS=$(nproc)
SDL_SRC=vendor/SDL

# fetch <url> <sha256> <file>: download once, refuse a file whose hash differs.
fetch() {
  if [ ! -f "$3" ] || ! echo "$2  $3" | sha256sum -c --status -; then
    curl -fsSL --retry 3 -o "$3.part" "$1"
    mv "$3.part" "$3"
  fi
  echo "$2  $3" | sha256sum -c -
  chmod +x "$3"
}

mkdir -p "$TOOLS" "$OUT_DIR"
fetch "$LINUXDEPLOY_URL" "$LINUXDEPLOY_SHA" "$TOOLS/linuxdeploy"
fetch "$APPIMAGETOOL_URL" "$APPIMAGETOOL_SHA" "$TOOLS/appimagetool"
fetch "$RUNTIME_URL" "$RUNTIME_SHA" "$TOOLS/runtime"
export APPIMAGE_EXTRACT_AND_RUN=1

# The vendored SDL3, configured exactly as debian/rules does.
cmake -S "$SDL_SRC" -B "$SDL_SRC/build" \
  -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=ON -DSDL_STATIC=OFF \
  -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
  -DCMAKE_INSTALL_PREFIX="$PWD/$SDL_SRC/install"
cmake --build "$SDL_SRC/build" -j"$JOBS"
cmake --install "$SDL_SRC/build"

# No APP_PATH: the binary finds its data through the overrides AppRun passes.
make -j"$JOBS" koncepcja
rm -rf "${APPDIR:?}"
make install DESTDIR="$PWD/$APPDIR" prefix=/usr SHARE_PATH=/usr/share/koncepcja
# make install also writes /etc/koncepcja.cfg, which an AppImage never reads.
rm -rf "${APPDIR:?}/etc"

LD_LIBRARY_PATH="$PWD/$SDL_SRC/install/lib" "$TOOLS/linuxdeploy" \
  --appdir "$APPDIR" \
  --executable "$APPDIR/usr/bin/koncepcja" \
  --desktop-file "$APPDIR/usr/share/applications/koncepcja.desktop" \
  --icon-file resources/freedesktop/icons/hicolor/256x256/apps/koncepcja.png \
  --custom-apprun scripts/appimage/AppRun

# The SDL3 the binary was linked against must be the one bundled.
test -f "$APPDIR/usr/lib/libSDL3.so.0"

# libcom_err is on the exclude list (e2fsprogs used to be on every system),
# but the bundled Kerberos libraries behind libcurl need it and Debian 13
# no longer installs it by default: without this copy the AppImage does not
# start there. The bundled libraries find it through their $ORIGIN rpath.
com_err=$(ldconfig -p | awk '$1 == "libcom_err.so.2" {print $NF; exit}')
test -n "$com_err"
cp -L "$com_err" "$APPDIR/usr/lib/libcom_err.so.2"

rm -f "$OUT"
ARCH=$ARCH "$TOOLS/appimagetool" --no-appstream --runtime-file "$TOOLS/runtime" \
  "$APPDIR" "$OUT"
ls -l "$OUT"
