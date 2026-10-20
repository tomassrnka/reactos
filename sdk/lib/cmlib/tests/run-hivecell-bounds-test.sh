#!/bin/sh
# Build and run the hive-cell-bounds regression test against the in-tree cmlib.
# Usage: run-hivecell-bounds-test.sh [output-binary]
# Exit code 0 = PASS; non-zero exit or a fault signal = FAIL.
set -e
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../../../.." && pwd)
CMLIB="$ROOT/sdk/lib/cmlib"
OUT=${1:-/tmp/hivecell_bounds_test}
CC=${CC:-cc}

"$CC" -std=gnu99 -fshort-wchar -Wno-multichar -g \
    -DCMLIB_HOST -DNASSERT -DTARGET_amd64 -D_NTSYSTEM_ -D__NO_CTYPE_INLINES -D__REACTOS__ \
    -I"$CMLIB" -I"$ROOT/sdk/include/host" \
    "$HERE/HiveCellBounds.c" \
    "$CMLIB"/hiveinit.c "$CMLIB"/hivecell.c "$CMLIB"/hivesum.c \
    -o "$OUT"

echo "built: $OUT"
"$OUT"
