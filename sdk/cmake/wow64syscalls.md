# WoW64 syscall metadata

The `wow64_syscall_tables` build target generates four X-macro includes and
matching CSV reports in `sdk/include/reactos/wow64` in the build directory.
They map NT and win32k service numbers in both directions by name. Each row
contains the source ID, target ID, both argument-slot counts and flags:

- `1`: no target service; the target ID is `0xffff`.
- `2`: different argument-slot counts; the thunk needs explicit review.

IDs include win32k's `0x1000` selector. Array order is source syscall order.
The NT input is `ntoskrnl/include/sysfuncs.h`, including its appended NT6 block,
as used by the kernel. The unused `sysfuncs64.h` is deliberately not an input.
The win32k inputs are `win32ss/w32ksvc32.h` and `w32ksvc64.h`; the generator
evaluates their `_WIN64` and `DBG` conditionals for each architecture. Use the
same `DBG` setting for the i386 user-mode build and the amd64 kernel build.

These are translation metadata, not callable thunks. Equal counts do not make
pointer-bearing structures, packed values or shared memory ABI-compatible.
Consumers must handle flagged entries and provide marshalling for each call.

The generator uses only CMake. For standalone use, pass `NT_SERVICES`,
`WIN32_SERVICES_32`, `WIN32_SERVICES_64`, `OUTPUT_DIR` and `DBG` with `-D`, then
`-P sdk/cmake/wow64syscalls.cmake`. `STRICT=ON` fails on flagged entries.
Unknown preprocessor directives, malformed entries, duplicate names and
out-of-range counts fail generation. Build dependencies regenerate the files
when an input changes. `wow64_syscall_tables_test` exercises reordered calls,
missing counterparts, count differences, checked/free numbering, deterministic
output and rejected input using isolated fixtures in the build directory.
