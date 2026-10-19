#!/bin/bash
# Dump the leaf layouts of the types named in probe_*.c with the real user32, gdi32 and imm32 compile flags.
#   regen.sh dump SRC B32 B64 OUT TAG native   i386 and amd64 layouts of the headers now in SRC
#   regen.sh dump SRC B32 B64 OUT TAG wide     i386 layouts with -DW32K_WIDE (OUT/<module>_i386wide_TAG.jsonl)
# SRC is the source tree, B32 and B64 are configured i386 and amd64 build trees of it, OUT collects the
# files. gdb, objcopy and python3 are required. Then regenerate the pinned tables from the dumps:
#   python3 gen_layout.py OUT/<m>_i386_BASE.jsonl OUT/<m>_amd64_BASE.jsonl OUT/<m>_i386wide_NEW.jsonl w32klayout_<m>.inc
# BASE dumps come from headers whose native layout is the baseline; NEW is the wide dump of the current headers.
set -eu
[ "$1" = dump ] || { echo "usage: see header" >&2; exit 2; }
SRC=$(realpath "$2"); B32=$(realpath "$3"); B64=$(realpath "$4"); OUT=$(realpath -m "$5"); TAG=$6; MODE=$7
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT"
declare -A OBJ=(
  [user]=win32ss/user/user32/CMakeFiles/user32.dir/misc/dllmain.c.obj
  [gdi]=win32ss/gdi/gdi32/CMakeFiles/gdi32.dir/misc/misc.c.obj
  [imm]=win32ss/user/imm32/CMakeFiles/imm32.dir/imm.c.obj)
probe() { # arch builddir module suffix extra-flags
  local arch=$1 B=$2 m=$3 suf=$4 extra=$5 cmd src srcdir o
  cmd=$(cd "$B" && ninja -t commands "${OBJ[$m]}" | tail -1)
  src=$(echo "$cmd" | grep -o -- '-c [^ ]*$' | cut -c4-)
  srcdir=$(cd "$B" && dirname "$(realpath "$src")")
  o=$OUT/${m}_${arch}${suf}_$TAG.o
  cmd=$(echo "$cmd" | sed -e "s# -o [^ ]*# -o $o#" -e "s# -c [^ ]*\$# -c $HERE/probe_$m.c#" \
      -e 's# -MD -MT [^ ]* -MF [^ ]*##' -e 's#-femit-struct-debug-detailed=none##' -e 's#-feliminate-unused-debug-symbols##' \
      -e 's# -include [^ ]*cmake_pch[^ ]*##g' -e 's# -Winvalid-pch##' -e 's# -gdwarf-2##' -e 's# -gstrict-dwarf##' -e 's# -g[0-9]\? # #' -e 's#^ccache ##' \
      -e "s#-gcc #-gcc -gdwarf-4 -fno-eliminate-unused-debug-types $extra #")
  (cd "$B" && eval "$cmd -I$srcdir -I$srcdir/include") > "$o.log" 2>&1 || { echo "probe failed: $o" >&2; grep -m5 error "$o.log" >&2; exit 1; }
  local f=$o
  if [ "$arch" = i386 ]; then objcopy -I pe-i386 -O elf32-i386 "$o" "$o.elf"; f=$o.elf; fi
  LAYOUT_OUT=$OUT/${m}_${arch}${suf}_$TAG.jsonl LAYOUT_PROBE=$HERE/probe_$m.c gdb -batch -x "$HERE/dump_layout.py" "$f" > "$o.gdb.log" 2>&1
  [ -s "$OUT/${m}_${arch}${suf}_$TAG.jsonl" ] || { echo "no layout dump: $o" >&2; exit 1; }
  ! grep -q '"error"' "$OUT/${m}_${arch}${suf}_$TAG.jsonl" || { echo "layout dump has error records: $OUT/${m}_${arch}${suf}_$TAG.jsonl" >&2; exit 1; }
}
for m in user gdi imm; do
  if [ "$MODE" = wide ]; then probe i386 "$B32" $m wide "-DW32K_WIDE"
  else probe i386 "$B32" $m "" ""; probe amd64 "$B64" $m "" ""; fi
done
echo "dumps in $OUT (tag $TAG, $MODE)"
