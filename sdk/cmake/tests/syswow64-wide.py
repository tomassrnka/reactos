#!/usr/bin/env python3
"""Check a Ninja SysWOW64 build's compile modes and staged GUI DLLs.

Run after building syswow64_stage:
    python3 sdk/cmake/tests/syswow64-wide.py path/to/amd64-build
Optionally add --native path/to/i386-build to check the native control too.
Only ReactOS build artifacts are inputs. Python is not a build dependency.
"""
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess


MODULES = {"user32", "gdi32", "imm32"}


def check_mode(build, guest):
    rows = json.loads(subprocess.check_output(
        ["ninja", "-C", str(build), "-t", "compdb"]))
    pins = set()
    rtl = set()
    checked = 0
    for row in rows:
        if not row["output"].endswith((".obj", ".gch")):
            continue
        match = re.search(r"CMakeFiles/([^/]+)\.dir/", row["output"])
        if not match:
            continue
        module = match[1]
        wide = "-DW32K_WIDE" in row["command"]
        assert wide == (guest and module in MODULES), row["output"]
        if module not in MODULES:
            continue
        checked += 1
        if Path(row["file"]).name == "w32klayout.c":
            pins.add(module)
            assert "-fno-ident" in row["command"], row["output"]
            assert "-include " not in row["command"], row["output"]
        if module == "user32" and "/user/rtl/" in row["file"]:
            rtl.add(Path(row["file"]).name)
    assert pins == MODULES, pins
    assert rtl == {"image.c", "text.c"}, rtl
    print(f"{build}: {checked} GUI compile commands, layout pins and rtl checked")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("--native", type=Path)
    parser.add_argument("--guest", type=Path,
                        help="override the nested guest directory for a baseline comparison")
    args = parser.parse_args()
    guest = args.guest or args.build / "syswow64-i386"
    check_mode(args.build, False)
    if args.native:
        check_mode(args.native, False)
    check_mode(guest, True)

    cache = (guest / "CMakeCache.txt").read_text()
    config = re.search(r"^CMAKE_BUILD_TYPE:STRING=(.*)$", cache, re.M)[1]
    rows = [line.split("|") for line in
            (guest / f"syswow64-guest.{config}.lst").read_text().splitlines()]
    live = (args.build / "syswow64" / "syswow64-livecd.lst").read_text().splitlines()
    for module in sorted(MODULES):
        name = module + ".dll"
        entries = [row for row in rows if row[:2] == ["SysWOW64", name]]
        assert len(entries) == 1, entries
        source = Path(entries[0][2])
        data = source.read_bytes()
        offset = struct.unpack_from("<I", data, 0x3c)[0]
        assert data[offset:offset + 4] == b"PE\0\0", source
        assert struct.unpack_from("<H", data, offset + 4)[0] == 0x14c, source
        staged = list((args.build / "syswow64" / "cab").glob(f"wow64-*-{name}"))
        assert len(staged) == 1 and staged[0].read_bytes() == data, staged
        assert live.count(f"reactos/SysWOW64/{name}={source}") == 1, name
        print(f"{name}: PE32 wide target staged once for cabinet and live image")


if __name__ == "__main__":
    main()
