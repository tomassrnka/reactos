#!/usr/bin/env python3
"""Compare ReactOS build artifacts only; run on the build host after building
ntdll, advapi32, ntdllsys, ntdllsys_nt6, win32ksys and win32u in both trees.
The baseline must be a native i386 build from before WoW64 (a6e8a8f2f25).
No Windows binary is an input to this regression test.
"""
import argparse
import hashlib
from pathlib import Path
import re
import struct
import subprocess


def text_section(path):
    data = path.read_bytes()
    count = struct.unpack_from('<H', data, 2)[0]
    optional_size = struct.unpack_from('<H', data, 16)[0]
    for i in range(count):
        pos = 20 + optional_size + i * 40
        if data[pos:pos + 8].rstrip(b'\0') == b'.text':
            size, offset = struct.unpack_from('<II', data, pos + 16)
            return data[offset:offset + size]
    raise AssertionError(f'{path}: no .text section')


def pe_headers(path):
    return subprocess.check_output(['objdump', '-p', str(path)], text=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('baseline', type=Path)
    parser.add_argument('native', type=Path)
    parser.add_argument('--guest', type=Path)
    args = parser.parse_args()
    errors = []
    for name in ('ntoskrnl/CMakeFiles/ntdllsys.dir/ntdll.S.obj',
                 'ntoskrnl/CMakeFiles/ntdllsys_nt6.dir/ntdll_nt6.S.obj',
                 'win32ss/CMakeFiles/win32ksys.dir/sys-stubs.S.obj',
                 'win32ss/win32u/CMakeFiles/win32u.dir/sys-stubs.S.obj'):
        before, after = (text_section(root / name) for root in (args.baseline, args.native))
        same = before == after
        print(f'{name}: {len(before)}/{len(after)} bytes, identical={same}, '
              f'sha256={hashlib.sha256(after).hexdigest()}')
        if not same:
            errors.append(name + ': native stub bytes changed')
    native_exports = pe_headers(args.native / 'dll/ntdll/ntdll.dll')
    if 'LdrInitializeWow64' in native_exports:
        errors.append('native ntdll exports LdrInitializeWow64')
    for root in (args.baseline, args.native):
        imports = re.findall(r'DLL Name: (\S+)', pe_headers(root / 'dll/win32/advapi32/advapi32.dll'))
        print(f'{root}: advapi32 imports={imports}')
        if root == args.baseline:
            baseline_imports = imports
        elif imports != baseline_imports:
            errors.append('native advapi32 import order changed')
    if args.guest:
        if 'LdrInitializeWow64' not in pe_headers(args.guest / 'dll/ntdll/ntdll.dll'):
            errors.append('guest ntdll lacks LdrInitializeWow64')
        guest = text_section(args.guest / 'ntoskrnl/CMakeFiles/ntdllsys.dir/ntdll.S.obj')
        if b'\x64\x8b\x15\xc0\x00\x00\x00' not in guest:
            errors.append('guest stubs lack fs:[0xc0] gate')
    for error in errors:
        print('FAIL:', error)
    if errors:
        raise SystemExit(1)
    print('Native stub bytes, loader exports and import order passed')


if __name__ == '__main__':
    main()
