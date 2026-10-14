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
The native loader recognizes this marker and enters the existing i386 loader
for TLS setup and DLL_THREAD_ATTACH. Guest ExitThread performs DLL_THREAD_DETACH
and FLS cleanup, and kernel thread rundown releases both stacks. Stack guard
faults update the TEB for the stack that faulted. Unmarked native helper threads
retain native startup.

Guest Get/SetThreadContext and the opt-in Wow64Get/SetThreadContext exports use
an access-checked kernel APC. It selects the compatibility-mode trap context,
saved translation state, or a suspended thread's captured startup context.
The trap helpers capture and restore x87 state only for the current guest
thread, without changing the creator's FPU when initializing a new thread.
Guest NtCreateThreadEx accepts suspended creation and client-ID/TEB output
attributes; other creation flags and attributes return STATUS_NOT_SUPPORTED.
Creation retains an internal handle for setup and returns a handle with the
requested access.

Guest APC routines are encoded separately from native routine addresses. The
native dispatcher builds an i386 APC frame and enters a nested CPU frame; its
NtContinue resumes the interrupted native call before returning to the guest.
Read/write I/O and supported device queries mark an IOSB32
pointer explicitly, and the kernel records that layout in the IRP. Completion
publishes the eight-byte status block before an event or APC becomes visible.
Filesystem redirection state remains per thread, including across nested APCs.
Completion-port thunks translate packet keys, contexts and IOSB32 results.
Kernel returns restore the native GS base for WoW64 threads after compatibility
code reloads a segment selector. The `wow64_segment_return` fixture exercises
exception dispatch and subsequent services on the main and a secondary thread.

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

* GUI services and callbacks are translated by `wow64win`; its coverage tables
  and README describe the implemented selectors and remaining limits.
* The initial guest stack and native translation stacks use fixed allocations.
  Secondary guest stacks can grow within the supplied reservation. Complete
  debugger and floating-point context compatibility still needs validation.
  The only CPU backend is amd64/i386.
* Device control accepts the mount-manager DOS-volume-path query, TCP entity
  queries and the AFD stream operations covered by `wow64_socket`. Accept
  handles and send/receive WSABUF arrays are widened in user mode; AFD captures
  them before returning pending. AFD owns native poll buffers through readiness,
  timeout and cancellation, then packs the guest output before completing the
  IOSB32. TDI handle replies and TCP query contexts use their guest layouts.
  Datagram-specific, event-select, extended accept/connect and shared-context
  retrieval requests still need translation. Other device controls and most
  information classes remain unimplemented. Pipe peeking and flushing accept
  synchronous file handles.
* CSR covers initial Base/console connection and the console operations
  implemented in `csr.c`. The Base static-data copy is a startup snapshot;
  live updates and INI mapping are not implemented. Console control falls
  back to termination; property and IME callbacks are unsupported.
* DLL discovery selects `SysWOW64` and `KnownDlls32`, retaining application and
  other search directories in their original order. Filesystem/registry
  redirection and environment translation cover the phase-2 console subset.
  Guest process creation resolves the executable in the guest and calls the
  native creator with captured parent parameters. System32, SysWOW64, Sysnative
  and the thread's redirection state select the executable architecture.
  Extended startup attributes remain unsupported. The installer fixtures cover
  additional file, token, COM and GUI paths; they are not complete API coverage.
  Guest current-directory references own their path buffer and handle together,
  retaining relative-path handles across directory changes. Explicit guest
  version layers also cover RtlGetNtVersionNumbers, which MSYS consults after
  RtlGetVersion; selecting a layer does not change the default system version.
  Object-directory enumeration repacks counted names and type names with guest
  pointers, including single-entry sizing, restart and continuation.
* i386 ntdll must map at its preferred base. Guest image relocation remains
  subject to the existing i386 loader's restrictions.

The feature requires independent review before integration. Runtime test
results and build/image provenance belong in the accompanying handoff.
