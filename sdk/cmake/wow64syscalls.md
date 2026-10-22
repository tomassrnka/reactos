# WoW64 syscall metadata

The `wow64_syscall_tables` build target generates four X-macro includes and
matching CSV reports in `sdk/include/reactos/wow64` in the build directory.
They map NT and win32k service numbers in both directions by name. Each row
contains the source ID, target ID, both argument-slot counts and flags:

- `1`: no target service; the target ID is `0xffff`.
- `2`: different argument-slot counts; the thunk needs explicit review.
- `4`: the row depends on a table conditional (`DBG` or `_WIN64`) on either side.
  `w32ksvc32.h` omits `UserSetWindowLongPtr` unless `_WIN64` and the `DbgWin32Heap*`,
  `SetDbgTag*` and `SetRipFlags` rows unless `DBG`; `w32ksvc64.h` always has them.
- `8`: a prototype argument is a structure or 64-bit value passed by value.
- `16`: with `2`, both tables agree with the prototypes and a by-value argument
  accounts for the difference (`POINT` and `SIZEL` take two slots on i386, one on amd64).
- `32`: a table slot count disagrees with the prototype.
- `64`: no usable prototype: not found, declared several ways, or an argument of unknown type.

IDs include win32k's `0x1000` selector. Array order is source syscall order.
The NT input is `ntoskrnl/include/sysfuncs.h`, including its appended NT6 block,
as used by the kernel. The unused `sysfuncs64.h` is deliberately not an input.
The win32k inputs are `win32ss/w32ksvc32.h` and `w32ksvc64.h`; the generator
evaluates their `_WIN64` and `DBG` conditionals for each architecture. Use the
same `DBG` setting for the i386 user-mode build and the amd64 kernel build.

## Argument descriptions

The host tool `wow64protos` (`sdk/tools/wow64protos`) reads the prototype of every
table row from `ntuser.h`, `psdk/ntgdi.h`, `ntgdibad.h`, `ntgdi/intgdi.h` and the NDK
`*funcs.h` headers, and writes `prototypes.csv`. It evaluates the `#if` family and
`#define` of those headers with three values: a condition it cannot decide keeps the
prototype and is reported (`_WIN64` is deliberately undecided, to describe both
architectures). The generator then writes three more files per build:

- `args.csv` and `args.inc`: one row per argument with its type, kind (`int`, `int64`,
  `ptrint`, `handle`, `pointer`, `struct`, `float` or `unresolved`), size and slots on i386
  and amd64, how amd64 passes a structure (`reg` for 1, 2, 4 or 8 bytes, `ref` otherwise)
  and the catalogue class when `-DCATALOG=a.tsv;b.tsv` names catalogue TSVs (the win32k thunk
  catalogue: call name in column 1, class in column 5).
- `calls.csv`: one row per call with its table slot counts, the counts the prototype needs
  (`slots_*_model`), the by-value argument count, the conditional and a status: `ok`,
  `by-value`, `table-mismatch`, `no-prototype`, `unresolved` or `ambiguous` (several prototypes, or a
  condition inside a parameter list that the tool cannot decide).

A slot is 4 bytes on i386 (a structure takes one slot per 4 bytes) and 8 bytes on amd64, where
every argument takes one slot: a structure of 1, 2, 4 or 8 bytes travels in the slot, a larger one
by reference. The kernel reads the amd64 count to copy stack arguments beyond the fourth slot, so
a count that is too low would lose arguments and one that is too high reads user stack that is not
an argument. The NT table has one count for both architectures (the i386 slots), which is an upper
bound on amd64.

The types come from `sdk/cmake/wow64types.txt`. An argument whose type is missing there is
reported as `unresolved` and left out of the check; add a line to resolve it. Every size in that file
was checked with a static assertion per type, compiled for i386 and for amd64 against the win32k and
NDK headers (the one exception, `LDT_ENTRY` on amd64, is noted on its line).

`WOW64_SYSCALL_CHECK` (ON by default) makes the generator fail the build when a table slot
count disagrees with its prototype: the message names the call, both counts and the by-value
arguments. Missing prototypes and unknown types are reported but never fail the build.

The descriptions stop at the argument: a pointer is one slot and its target is not described, so a
structure that differs between the architectures behind a pointer (a `UNICODE_STRING`, a `cbSize` gate,
an out-buffer whose size depends on the architecture) is not seen here; the catalogue class names such calls.

These are translation metadata, not callable thunks. Equal counts do not make
pointer-bearing structures, packed values or shared memory ABI-compatible.
Consumers must handle flagged entries and provide marshalling for each call.

## Standalone use

The generator uses only CMake; the prototype reader is a C host tool. For standalone use run
`wow64protos -t sdk/cmake/wow64types.txt -s <table.h>... -o prototypes.csv <headers>...`
(`-D`/`-U` set the macros the headers see), then pass `NT_SERVICES`, `WIN32_SERVICES_32`,
`WIN32_SERVICES_64`, `OUTPUT_DIR`, `DBG` and optionally `PROTOTYPES`, `CATALOG`, `CHECK`
with `-D`, then `-P sdk/cmake/wow64syscalls.cmake`. `STRICT=ON` fails on flagged entries.
Unknown preprocessor directives, malformed entries, duplicate names and
out-of-range counts fail generation. Build dependencies regenerate the files
when an input changes. `wow64_syscall_tables_test` exercises reordered calls,
missing counterparts, count differences, conditional rows, checked/free numbering,
deterministic output, rejected input, the prototype reader and the check using
isolated fixtures in the build directory.

Phase 3c must consume `sdk/cmake/wow64handles.cmake` before resolving typedefs.
Each pipe-delimited row gives the declared type, family, 32-to-64 extension and
64-to-32 truncation. `SIGN` means `(LONG64)(LONG)(ULONG)value`; `ZERO` means
`(ULONG64)(ULONG)value`; `TRUNCATE` keeps the low 32 bits. USER, NT and HKL handles
sign-extend, including negative pseudo-handles. GDI and address-valued
handles zero-extend. The GDI rule is specific to ReactOS; Windows issues
bit-31 GDI handles sign-extended and accepts both forms. GDI handles stored
in DC_ATTR and batch records also use zero extension, matching win32k's
issued handles and full-width comparisons.

This is an explicit table, not an H-prefix heuristic. An unknown type requires
review. Generic HANDLE parameters need their semantic family checked: for
example NtGdiDeleteObjectApp's HANDLE is a GDI handle, not an NT handle. Pointer
aliases such as PHANDLE describe pointed-to fields; the pointer itself always
zero-extends. Do not classify HDEV, HRAWINPUT or other opaque values by name.

LPARAM/LRESULT sign-extend and WPARAM zero-extends under the separate integer
table. Message-specific thunks can recover a known payload type; otherwise
win32k accepts the low 32 bits for residual GDI lookups in WoW64-enabled
amd64 images. HKL keeps full-width equality: typed HKL arguments and HKLs in
LPARAM both arrive sign-extended, as Windows requires. Default native builds
retain their previous comparisons.

See [the handle measurements](wow64handles.md) for the Windows reference results
and the limits of the step-3b tests.
