#!/bin/sh
# build-standalone.sh [OUTDIR]: build LocaleTable.c as a stand-alone program with the system MinGW (not RosBE)
# to run it on Windows, where it must report no failure: the expectations come from there.
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-.}
tmp=$(mktemp -d)
cp "$here/shim/precomp.h" "$here/../LocaleTable.c" "$here/../LocaleTable.h" "$tmp/"
x86_64-w64-mingw32-gcc -O1 -Wall -Wno-unused-function -o "$out/LocaleTable64.exe" "$tmp/LocaleTable.c"
i686-w64-mingw32-gcc -O1 -Wall -Wno-unused-function -o "$out/LocaleTable32.exe" "$tmp/LocaleTable.c"
rm -rf "$tmp"
