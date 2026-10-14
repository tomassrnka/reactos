# Console WoW64 comparisons

The three `*_wow64_apitest` targets compile selected kernel32, ntdll and
advapi32 API-test sources with the ordinary Wine test runner. This keeps the
console comparison independent of unrelated GUI/network imports in the full
API-test executables. The full targets remain available.

Build the targets for i386 and run **the same executable files** on native i386
and amd64 with WoW64 enabled. Use the same i386 runtime DLLs on both systems,
set `WINETEST_PLATFORM=reactos`, and record individual test summaries and exits.
An unsupported process launch or missing summary is not a passing test.

The explicit i386 target `wow64_gui_load` is a separate GUI DLL load smoke
test and is not installed in default images. Run it on the WoW64 image to
load user32, gdi32 and imm32 through the ordinary loader, print their loaded
paths, and call SetRect and GdiGetBatchLimit without creating a window. DLL
initialization itself requires win32k translation; a failure here remains
a failure even when the wide DLLs compile and are packaged correctly.
The host-side `sdk/cmake/tests/syswow64-wide.py` checks the Ninja compile
commands (including layout pins and shared rtl sources), guest-only wide
definitions, and the cabinet/live-image copies after `syswow64_stage`.

The selection covers current directories, environment variables, drive types,
code pages, integer SEH, NT registry queries, ACL-bearing registry creation,
registry enumeration and value queries. `Wow64Environment` adds process
queries and filesystem views; `RegWow64` checks isolated and shared registry
keys, root-relative view selection and view-specific deletion. Also run
`RegWow64` as a native amd64 executable to check explicit 32-bit selectors.

`NtQueryKey` checks the physical Wow6432Node name when running under WoW64.
Its existing assumption of at least five software subkeys depends on image
contents. `RtlDetermineDosPathNameType` reports a skip if the optional
`RtlDetermineDosPathNameType_Ustr` export is absent. Keep these residuals in
the comparison rather than suppressing assertions or classifying skips as
passes.

This selection does not establish general service, RPC, network or GUI
support. Guest APCs, additional threads, software exception raising and
floating-point/debug exception contexts require further work. The registry policy follows the Windows 7 shared/redirected matrix for
machine Software and machine/per-user Classes, including the five redirected
COM/media subtrees. Windows 7 does not reflect registry writes. General
registry symbolic-link view propagation remains a separate gate.

Review regression fixtures cover integer continuation and native partial
contexts, Unicode/compressed registry ancestry, native object-name length
bounds, physical roots, nested view creation, repeated separators, volatile
Classes and the shared/redirected matrix. Environment fixtures cover redirected
delete, all six shared directories, native filesystem controls, architecture
variables, installed Program Files values and native environment-block creation.
Native calls without registry selectors are explicit controls; native i386
continues to ignore selectors. The additional guest-thread limitation means
these tests do not claim concurrent guest redirection-isolation coverage.
