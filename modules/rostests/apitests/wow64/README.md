# Console WoW64 comparisons

The three `*_wow64_apitest` targets compile selected kernel32, ntdll and
advapi32 API-test sources with the ordinary Wine test runner. This keeps the
console comparison independent of unrelated GUI/network imports in the full
API-test executables. The full targets remain available.

Build the targets for i386 and run **the same executable files** on native i386
and amd64 with WoW64 enabled. Use the same i386 runtime DLLs on both systems,
set `WINETEST_PLATFORM=reactos`, and record individual test summaries and exits.
An unsupported process launch or missing summary is not a passing test.

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
floating-point/debug exception contexts require further work. The basic
registry policy redirects HKLM Software and shares HKCU, HKLM SYSTEM and
Software Classes; COM subviews, the remaining Windows 7 key exceptions,
reflection and registry symbolic-link view propagation are separate gates.
