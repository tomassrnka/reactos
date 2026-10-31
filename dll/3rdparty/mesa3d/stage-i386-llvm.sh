#!/bin/bash
# Stage the i386 llvmpipe OpenGL driver (LLVM build of libgallium_wgl.dll, ~19 MB download, 73 MB installed) into
# dll/3rdparty/mesa3d/i386-llvm/ so that the SysWOW64 part of an amd64 image gives 32-bit programs llvmpipe
# (OpenGL 4.6) like the 64-bit side, instead of the in-tree softpipe (OpenGL 3.3, no compute shaders).
#
#   stage-i386-llvm.sh [OUTDIR]        (default: i386-llvm/ next to this script)
#
# Built by build-mesa.sh with ARCHES=i386 I386_LLVM=1; mesa3d-licenses.txt of the pack has the LLVM licence appended.
# Fetched from the S3 cold tier (rclone remote "vultr") and verified like stage-amd64-llvm.sh; set
# LLVMPIPE_I386_ARTIFACT=/path/to/wow64-llvmpipe-i386-artifacts.tar.xz to use a local copy. Exit 0 = staged and
# verified; any failure exits non-zero without leaving a partial i386-llvm/, and the build keeps softpipe.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/i386-llvm}
ARTIFACT=s3://reactos-nitro-archive/wow64-llvmpipe-i386/wow64-llvmpipe-i386-artifacts.tar.xz
ARTIFACT_SHA256=c1e739e70ae73d6617814cf54555c7620e735b94cd1e87a4c393df99df2a6fab
FILES="libgallium_wgl.dll mesa3d-licenses.txt"

mkdir -p "$(dirname "$OUT")"
exec 9>"$HERE/.stage-i386-llvm.lock"
flock 9

if [ -f "$OUT/SHA256SUMS" ] && ( cd "$OUT" && sha256sum -c SHA256SUMS >/dev/null 2>&1 ); then
  echo "stage-i386-llvm: i386-llvm/ already staged and verified"
  exit 0
fi

TMP=$(mktemp -d)
trap "rm -rf \"$TMP\"" EXIT
if [ -n "${LLVMPIPE_I386_ARTIFACT:-}" ]; then
  cp "$LLVMPIPE_I386_ARTIFACT" "$TMP/a.tar.xz"
else
  command -v rclone >/dev/null 2>&1 || { echo "stage-i386-llvm: rclone not found (set LLVMPIPE_I386_ARTIFACT=<local tar>)" >&2; exit 3; }
  REM=${ARTIFACT#s3://}; REM=vultr:${REM}
  rclone copyto "$REM" "$TMP/a.tar.xz"
fi
echo "$ARTIFACT_SHA256  $TMP/a.tar.xz" | sha256sum -c -
tar -xJf "$TMP/a.tar.xz" -C "$TMP"
( cd "$TMP/i386-llvm" && sha256sum -c SHA256SUMS )
rm -rf "$OUT"; mkdir -p "$OUT"
for f in $FILES SHA256SUMS; do install -m 0644 "$TMP/i386-llvm/$f" "$OUT/$f"; done
( cd "$OUT" && sha256sum -c SHA256SUMS >/dev/null )
echo "stage-i386-llvm: staged the i386 llvmpipe driver in $OUT"
