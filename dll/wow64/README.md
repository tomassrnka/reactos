# Experimental i386 runtime on amd64

Configure the amd64 GCC build with `-DBUILD_WOW64_ENABLED=ON`. This also
selects `SYSWOW64` by default. The latter configures a nested i386 build
with the same Debug/Release and DLL export settings and installs its ntdll
and kernel32 into `SysWOW64`. Ordinary i386 builds do not need this option.
The runtime is disabled by default.

The phase-1 target is a single-threaded PE32 console executable. The fixture
`wow64_hello` imports the ordinary i386 kernel32 and ntdll libraries and
uses Win32 console/file and virtual-memory APIs. Build it in an i386 test
build, install it into `SysWOW64` on amd64 (or `system32` on i386), and run
`kernel32_apitest Wow64Run` from a native console process. The test checks
bitness, suspended startup, exact redirected output, actual console-screen
contents, repeated process creation, exit status, pointer marshalling and
error paths. It uses the same PE32 executable for the native i386 control.

`kernel32_apitest Wow64ReviewFixes` uses `wow64_native.exe` from native
`system32`, and the i386 `wow64_errors.exe`, `wow64_localhello.exe` and
`wow64_local.dll` from `SysWOW64` (all from `system32` on native i386).
It tests native thread startup after discarding the main PE signature,
faulting handle and memory outputs without leaked objects or mappings,
and a PE32 executable with an adjacent static-import DLL. The latter pair
is copied to a temporary application directory before launch.

## Entry and lifetime

The kernel's process-owned WoW64 object identifies an i386 PE32 main image
and provides low-address PEB32/TEB32 storage. The native loader initializes
its own heap and module lists without interpreting PE32 imports, TLS or
pointer-sized optional-header fields as native fields. It then loads
`wow64.dll`, which constructs PEB32, process parameters, NLS data and TEB32,
maps i386 ntdll, and enters its loader on a separate low-address stack.
The ordinary i386 loader loads kernel32 and the executable's imports.

The i386 syscall stubs check `TEB.WOW32Reserved`. A zero value retains the
native i386 shared-page dispatcher. A WoW64 thread uses a read/execute gate
which makes a far jump into 64-bit user mode, then jumps to `wow64cpu.dll`.
The native TEB reserves TLS slot 1 for the CPU state. Each call switches to
the thread's native user stack, marshals arguments in `wow64.dll`, calls
native ntdll, and returns to the i386 stub. Native ntdll uses LSTAR as usual.
Raw compatibility-mode CSTAR calls continue to return
`STATUS_INVALID_SYSTEM_SERVICE`. The gate does not depend on AMD's
compatibility-mode SYSCALL instruction.

The service IDs come from the generated NT mapping, not a copied Windows
or Wine numbering table. Each implemented service has an explicit typed
case; equal argument counts never authorize generic forwarding. Pointer
and SIZE_T outputs, handles, strings, object attributes, section information,
virtual-memory information and synchronous I/O status blocks are converted
explicitly. Unsupported services and information classes return errors.
Pointer captures run under native user-mode exception handling.
Translated output destinations are checked before native side effects.
New handles, reservations and mapped views are released if their translated
outputs cannot be copied back. Existing caller-owned reservations remain
owned by the caller; changes to existing memory and file contents are not
transactional against concurrent changes to output memory.

CSR uses a separate private user-mode operation on the same gate. The i386
CSR library retains local capture buffers; the native bridge constructs
native Base/console connection messages and native captures for console
writes. The server continues to receive its native ABI. Base static data
is copied into an explicitly sized i386 representation, with a read-only
structure and pointer table. The ordinary i386 kernel32 console code runs.

The mode-switch implementation is original ReactOS code. Wine's public
`dlls/wow64cpu/cpu.c` was consulted as an architectural reference; no Wine
implementation was copied into these DLLs.

## Current limits

This is the phase-1 runtime, not general WoW64 compatibility:

* Only initial-thread startup is supported. Thread creation, guest APCs,
  guest SEH, win32k callbacks and GUI services need later work. Unhandled
  compatibility-mode faults and explicit guest exception raises terminate
  with the exception status. Native thunk captures use native SEH normally.
* The guest stack is a fixed allocation. There is no stack growth, guest
  context/debugger API, or CPU backend other than amd64/i386.
* I/O with user APCs, event completion or asynchronous handles is rejected.
  Pointer-bearing IOCTLs and most information classes are unimplemented.
* CSR covers initial Base/console connection and the console operations
  implemented in `csr.c`. The Base static-data copy is a startup snapshot;
  live updates and INI mapping are not implemented. Console control falls
  back to termination; property and IME callbacks are unsupported.
* DLL discovery selects `SysWOW64` and `KnownDlls32` for the i386 loader,
  retaining application and other search directories in their original order.
  General filesystem/registry redirection, environment rewriting, arbitrary
  native/guest process creation, services and installers are not implemented.
* i386 ntdll must map at its preferred base. Guest image relocation remains
  subject to the existing i386 loader's restrictions.

The feature requires independent review before integration. Runtime test
results and build/image provenance belong in the accompanying handoff.
