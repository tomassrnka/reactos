#!/bin/bash
# Stage the hosted lavapipe-pack payload (GFX-01 artifact, ~116 MB) for FEATMGMT-01.
#   stage-lavapipe-payload.sh <outdir>
# Produces <outdir>/{vulkan_lvp.dll,lvp_icd.json,libgallium_wgl.dll,llvm-licenses.txt,mesa3d-licenses.txt}
# which is copied to %SystemRoot%\Featmgr\Payload\lavapipe-pack\ on the target. The artifact
# location and hash are the [Source] section of catalog/lavapipe-pack.ini (single source of truth).
set -euo pipefail
OUT=${1:?usage: $0 <outdir>}
HERE=$(cd "$(dirname "$0")" && pwd)
INI=$HERE/../catalog/lavapipe-pack.ini
ART=$(sed -n "s/^Artifact=//p" "$INI" | tr -d "\r")
SHA=$(sed -n "s/^ArtifactSHA256=//p" "$INI" | tr -d "\r")
REM=${ART#s3://}; REM=vultr:${REM}          # rclone remote "vultr" = Vultr ams2 object storage
TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
if [ -n "${LAVAPIPE_ARTIFACT:-}" ]; then cp "$LAVAPIPE_ARTIFACT" "$TMP/a.tar.xz"; else rclone copyto "$REM" "$TMP/a.tar.xz"; fi
echo "$SHA  $TMP/a.tar.xz" | sha256sum -c -
mkdir -p "$OUT"; tar -xJf "$TMP/a.tar.xz" -C "$TMP"
( cd "$TMP/amd64-llvm" && sha256sum -c SHA256SUMS )
for f in vulkan_lvp.dll lvp_icd.json libgallium_wgl.dll llvm-licenses.txt mesa3d-licenses.txt; do
  install -m 0644 "$TMP/amd64-llvm/$f" "$OUT/$f"
done
echo "staged lavapipe-pack payload in $OUT"
