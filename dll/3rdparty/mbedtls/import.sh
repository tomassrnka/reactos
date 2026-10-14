#!/bin/sh
# Imports a pinned Mbed TLS release into this tree.
# Usage: sh import.sh [path-to-tarball]   (run from dll/3rdparty/mbedtls; downloads the tarball when none is given)
set -e
VERSION=3.6.7
URL=https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$VERSION/mbedtls-$VERSION.tar.bz2
SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
INC=$ROOT/sdk/include/reactos/libs
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

TARBALL=$1
if [ -z "$TARBALL" ]; then
    TARBALL=$WORK/mbedtls-$VERSION.tar.bz2
    curl -fsSL -o "$TARBALL" "$URL"
fi
echo "$SHA256  $TARBALL" | sha256sum -c -

tar -xjf "$TARBALL" -C "$WORK"
SRC=$WORK/mbedtls-$VERSION

# Files owned by ReactOS stay: CMakeLists.txt, mbedtls.rc, mbedtls.spec, import.sh, ros_platform.c, ros_config.h
find "$HERE" -maxdepth 1 -type f \( -name '*.c' -o -name '*.h' \) ! -name 'ros_*' -delete
rm -f "$INC"/mbedtls/*.h "$INC"/psa/*.h
mkdir -p "$INC/mbedtls" "$INC/psa"
cp "$SRC"/library/*.c "$SRC"/library/*.h "$HERE"/
cp "$SRC"/include/mbedtls/*.h "$INC/mbedtls/"
cp "$SRC"/include/psa/*.h "$INC/psa/"
cp "$HERE/ros_config.h" "$INC/mbedtls/mbedtls_config.h"
cp "$HERE/ros_threading_alt.h" "$INC/mbedtls/threading_alt.h"
patch -d "$HERE" -p0 < "$HERE/ros_hrr_cookie.patch"
echo "Imported Mbed TLS $VERSION"
