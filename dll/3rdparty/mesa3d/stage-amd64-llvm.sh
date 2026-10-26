#!/bin/bash
# Stage the amd64 LLVM Mesa pack (llvmpipe OpenGL + lavapipe Vulkan, GFX-01 artifact: ~35 MB
# download, ~116 MB installed) into dll/3rdparty/mesa3d/amd64-llvm/ so that CMakeLists.txt makes
# llvmpipe the DEFAULT OpenGL driver and installs the lavapipe Vulkan ICD in the base image.
#
#   stage-amd64-llvm.sh [OUTDIR]        (default: amd64-llvm/ next to this script)
#
# The binaries are NOT in git (66-69 MB each). They are fetched from the S3 cold tier (rclone
# remote "vultr" = Vultr ams2 object storage) and verified against SHA256SUMS. Set
# LAVAPIPE_ARTIFACT=/path/to/gfx01-lavapipe-amd64-artifacts.tar.xz to use a local copy instead
# of downloading (offline / CI mirror). The artifact + hash are the single source of truth shared
# with base/applications/featmgr/catalog/lavapipe-pack.ini.
#
# Exit 0 = amd64-llvm/ populated and verified (or already was). Any failure exits non-zero WITHOUT
# leaving a partial amd64-llvm/, and the build then keeps the in-tree softpipe OpenGL default
# (CMakeLists.txt falls back), so a host without rclone / network / the artifact still builds.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/amd64-llvm}
ARTIFACT=s3://reactos-nitro-archive/gfx01-lavapipe-amd64/gfx01-lavapipe-amd64-artifacts.tar.xz
ARTIFACT_SHA256=6ab3d0fed744d5e9b9c2dca11c2d6a86ab1291533b49a6e693bb22729c84d345
FILES="vulkan_lvp.dll lvp_icd.json libgallium_wgl.dll llvm-licenses.txt mesa3d-licenses.txt"

# serialize concurrent configures of a shared source tree (amd64 Debug + amd64 Release)
mkdir -p "$(dirname "$OUT")"
exec 9>"$HERE/.stage-amd64-llvm.lock"
flock 9

# already staged and verified? then do nothing (idempotent, no network)
if [ -f "$OUT/SHA256SUMS" ] && ( cd "$OUT" && sha256sum -c SHA256SUMS >/dev/null 2>&1 ); then
  echo "stage-amd64-llvm: amd64-llvm/ already staged and verified"
  exit 0
fi

TMP=$(mktemp -d)
trap "rm -rf \"$TMP\"" EXIT
if [ -n "${LAVAPIPE_ARTIFACT:-}" ]; then
  cp "$LAVAPIPE_ARTIFACT" "$TMP/a.tar.xz"
else
  command -v rclone >/dev/null 2>&1 || { echo "stage-amd64-llvm: rclone not found (set LAVAPIPE_ARTIFACT=<local tar>)" >&2; exit 3; }
  REM=${ARTIFACT#s3://}; REM=vultr:${REM}
  rclone copyto "$REM" "$TMP/a.tar.xz"
fi
echo "$ARTIFACT_SHA256  $TMP/a.tar.xz" | sha256sum -c -
tar -xJf "$TMP/a.tar.xz" -C "$TMP"
( cd "$TMP/amd64-llvm" && sha256sum -c SHA256SUMS )
mkdir -p "$OUT"
for f in $FILES SHA256SUMS; do install -m 0644 "$TMP/amd64-llvm/$f" "$OUT/$f"; done
( cd "$OUT" && sha256sum -c SHA256SUMS >/dev/null )
echo "stage-amd64-llvm: staged llvmpipe/lavapipe pack in $OUT"
