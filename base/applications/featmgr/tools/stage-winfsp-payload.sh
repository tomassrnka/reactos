#!/bin/bash
# Stage the hosted WinFsp payload (~0.2 MB) for FEATMGMT (feature "winfsp").
#   stage-winfsp-payload.sh <outdir>
# Produces <outdir>/{winfsp-x64.sys,winfsp-x64.dll,memfs-x64.exe,winfsp-licenses.txt}, to be copied to
# %SystemRoot%\Featmgr\Payload\winfsp\ on the target. Artifact location + hash are the [Source]
# section of catalog/winfsp.ini (single source of truth). WINFSP_ARTIFACT=<file> skips the download.
set -euo pipefail
OUT=${1:?usage: $0 <outdir>}
HERE=$(cd "$(dirname "$0")" && pwd)
INI=$HERE/../catalog/winfsp.ini
ART=$(sed -n "s/^Artifact=//p" "$INI" | tr -d "\r")
SHA=$(sed -n "s/^ArtifactSHA256=//p" "$INI" | tr -d "\r")
REM=${ART#s3://}; REM=vultr:${REM}          # rclone remote "vultr" = Vultr ams2 object storage
TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
if [ -n "${WINFSP_ARTIFACT:-}" ]; then cp "$WINFSP_ARTIFACT" "$TMP/a.tar.xz"; else rclone copyto "$REM" "$TMP/a.tar.xz"; fi
echo "$SHA  $TMP/a.tar.xz" | sha256sum -c -
mkdir -p "$OUT"; tar -xJf "$TMP/a.tar.xz" -C "$TMP"
( cd "$TMP/winfsp-x64" && sha256sum -c SHA256SUMS )
for f in winfsp-x64.sys winfsp-x64.dll memfs-x64.exe winfsp-licenses.txt; do
  install -m 0644 "$TMP/winfsp-x64/$f" "$OUT/$f"
done
echo "staged winfsp payload in $OUT"
