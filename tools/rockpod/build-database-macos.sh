#!/bin/sh
# Build the ipod6g database host tool on macOS.  The Rockbox configure script
# emits an ARM-style Makefile even for --type=d, so the compiler/flag
# corrections below are applied only to this host build.
set -eu
src=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build=${1:-"$src/build-ipod6g-database"}
mkdir -p "$build"
cd "$build"
"$src/tools/configure" --target=ipod6g --type=d
sed -i '' \
  -e 's#^export CC=.*#export CC=/usr/bin/clang#' \
  -e 's#^export HOSTCC=.*#export HOSTCC=/usr/bin/clang#' \
  -e 's#^export CPP=.*#export CPP=/usr/bin/clang -E#' \
  -e 's#^export AR=.*#export AR=/usr/bin/ar#' \
  -e 's/ -funit-at-a-time//g' \
  -e 's/ -Wimplicit-fallthrough=0//g' \
  Makefile
make -j"$(sysctl -n hw.ncpu)"
file database.ipod6g | grep -q 'Mach-O' || {
    echo "database.ipod6g is not a native macOS executable" >&2
    exit 1
}
