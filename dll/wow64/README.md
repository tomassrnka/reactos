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

## Guest threads

Thread creation captures the guest context and allocates a separate native
translation stack. The native thread starts suspended with a startup marker;
TEB32 and its guest stack bounds are installed before the creator can resume it.
The native loader identifies its process initializer explicitly. Guest secondary
threads wait for a release/acquire publication of the complete WoW64 process
state; native helpers return without entering guest initialization.
The native loader recognizes this marker and enters the existing i386 loader
for TLS setup and DLL_THREAD_ATTACH. Guest ExitThread performs DLL_THREAD_DETACH
and FLS cleanup, and kernel thread rundown releases both stacks. Stack guard
faults update the TEB for the stack that faulted. Unmarked native helper threads
retain native startup.

Forced thread termination still releases both stacks. Matching native i386's
retained stack requires changing post-creation failure cleanup ownership as
well; otherwise those paths leak the allocations they currently leave to
thread rundown. That compatibility change remains open.

Guest Get/SetThreadContext and the opt-in Wow64Get/SetThreadContext exports use
an access-checked kernel APC. It selects the compatibility-mode trap context,
saved translation state, or a suspended thread's captured startup context.
During either mode transition the saved state is not authoritative: requests
return STATUS_RETRY without changing the output or the target. A controller
must resume a suspended thread before retrying. The transition flag remains
set across IRET, guest execution and the entire next entry, and is cleared only
after all saves are complete in the translator. Direct guest NtContinue paths
also set it. Integer changes accepted inside a translator preserve the changed
EAX when the system service completes; an unchanged EAX round trip preserves
the actual service result. APC frame cloning and restoration remain marked
until the authoritative frame has been published in TLS.
The trap helpers capture and restore x87 state only for the current guest
thread, without changing the creator's FPU when initializing a new thread.
Guest NtCreateThreadEx accepts suspended creation and client-ID/TEB output
attributes; other creation flags and attributes return STATUS_NOT_SUPPORTED.
Creation retains an internal handle for setup and returns a handle with the
requested access.

Guest APC routines are encoded separately from native routine addresses. The
native dispatcher builds an i386 APC frame and enters a nested CPU frame; its
NtContinue resumes the interrupted native call before returning to the guest.
Read/write I/O marks an IOSB32 pointer explicitly, and the kernel records that
layout in the IRP. Completion publishes the eight-byte status block before an
event or APC becomes visible. IRP_WOW64_IO_STATUS reserves Flags bit 31 in
opt-in amd64 builds; drivers forwarding an IRP must preserve it and must not
copy it onto an IRP with a different UserIosb layout. Encoded guest APCs on a
native-only thread are dropped, since that thread has no guest CPU state.
Filesystem redirection state remains per thread,
including across nested APCs.
Completion-port thunks translate packet keys, contexts and IOSB32 results.

The fixtures Wow64Threads, QueueUserAPC, Wow64ThreadApc and
Wow64ThreadLifetime cover stress, TLS/FLS, loader notifications, redirection
isolation, secondary SEH, asynchronous read/write completion, completion ports,
fibers, integer/x87 context and thread lifetime. Wow64RemoteThread uses a native
controller and two PE32 children to check cross-process creation and context
changes. Wow64ThreadExitProcess emits a marker before exiting with live
workers; its external runner must check both that marker and exit code zero.
Use identical PE32 fixtures for native i386 and WoW64 comparisons.

## Current limits

This remains an experimental runtime; the full Wine compatibility gate is
separate from the focused fixtures:

* win32k callbacks and GUI services are not implemented. The complete ntdll
  exception winetest imports user32 and exercises GUI callbacks.
* The initial guest stack uses a fixed allocation. Each secondary native stack
  reserves 1 MB but initially commits 64 KB including a guard page, with native
  stack growth. Reservations still consume the process's low address space.
  Secondary guest stacks can grow within the supplied reservation. Complete
  debugger and floating-point context compatibility still needs validation.
  The only CPU backend is amd64/i386.
* Pointer-bearing IOCTLs and most information classes are unimplemented.
  Asynchronous I/O support here covers NtReadFile/NtWriteFile; other I/O APIs
  retain their existing restrictions.
* CSR covers initial Base/console connection and the console operations
  implemented in `csr.c`. The Base static-data copy is a startup snapshot;
  live updates and INI mapping are not implemented. Console control falls
  back to termination; property and IME callbacks are unsupported.
* DLL discovery selects `SysWOW64` and `KnownDlls32`, retaining application and
  other search directories in their original order. Filesystem/registry
  redirection and environment translation cover the phase-2 console subset.
  Arbitrary guest-initiated process creation, services and installers remain
  outside that subset.
* i386 ntdll must map at its preferred base. Guest image relocation remains
  subject to the existing i386 loader's restrictions.

The feature requires independent review before integration. Runtime test
results and build/image provenance belong in the accompanying handoff.

`Wow64ReviewThreads` adds native helper exits during guest allocation churn and
an encoded-APC check on a native-only helper. `phase3a_review.py` under the
WoW64 apitests instruments disposable builds for deterministic startup and
instruction-boundary context schedules and observes stack release calls. The
same probes apply to the reviewed and fixed sources. Published binaries and
the ordinary pass-bar runs must be rebuilt without these probes.
