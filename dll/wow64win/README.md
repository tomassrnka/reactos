# ReactOS win32k translation

This module is built and installed only with BUILD_WOW64_ENABLED. Native
system-service stubs follow w32ksvc64.h; guest dispatch IDs follow the i386
view of w32ksvc32.h. Native user32 is not loaded into the guest process.

`generate.py` is a maintainer tool, not a build dependency. Its checked-in
outputs translate catalogue A/B calls using declared argument types, the
phase-3b handle table, and measured RosBE layouts. Shared wide structures
are native-layout buffers; private syscall and callback buffers remain
ordinary i386 structures. Missing measured layouts require an explicit
reviewed invariant entry. Unknown carriers and hook-dependent arguments
are unsupported. `coverage.tsv` records every catalogue service, including
services with no guest entry. Generated coverage is implementation coverage,
not evidence that every service has passed runtime tests.

`user.c` handles creation, class registration, messages, painting, wide
process connection and selector-dependent calls. `userextra.c` translates
menus, system parameters, cursor/icon metadata, clipboard queries and
control information. `gdi.c` translates DIB sections, object queries and
the StartDoc stub. `callback.c` creates a
nested CPU frame on the native call stack, packs arguments on the guest
stack, enters i386 ntdll's callback dispatcher, and widens returned data
before NtCallbackReturn. The previous CPU frame and guest exception chain
are restored on ordinary callback return. Creation strings are packed
relative to their translated CREATESTRUCT, matching ReactOS user32.

Cross-callback RtlUnwind transfers the active guest continuation to the
surviving CPU frame as each native callback is popped. Exception-chain
removal remains with the i386 unwinder. Callback allocation touches guest
stack pages in order to grow the guard page, preserving the reserved stack
guarantee. Normal returns preserve changes to x87 and MXCSR control state.

Current limits include global/other-thread hooks and hook families other than
CALLWNDPROC/CALLWNDPROCRET, DDE conversation callbacks, IME callbacks,
remaining private structures, printer driver pointers, and unconverted
pointer messages. DDE initialization and string handles work; StartDoc
retains the native service's stub result. Arbitrary
application-defined messages retain their normal integer transport; the
application must define its own cross-bitness protocol.

The explicit `wow64_first_window` and `wow64_win32k_abi` targets are excluded
from normal images. Run the same PE32 executables on i386 and WoW64. The GUI
fixture requires an external mouse click and checks creation payloads,
painting, button notification, destruction and class unregistration.
Additional explicit fixtures cover four exception/unwind paths, a 32-position
stack-boundary sweep, menus/edit/list/combobox controls, clipboard, timers,
common dialogs and a native driver for the guest notepad/regedit executables.
All remain excluded from default image contents. The unwind fixture can run
its child cases through guest CreateProcess or accept individual case numbers.
Installer fixtures also cover outline-font offsets, input contexts, monitor
handles, the thread desktop, and CALLWND hook payloads. Hooks are currently
limited to the calling guest thread; their message payload coverage follows
the window-message translator.

Regenerate with `python3 dll/wow64win/generate.py`. On the build host, run
`TMPDIR=<scratch-directory> python3 dll/wow64win/tests/generator.py <source>`
to verify determinism, handle-policy propagation, rejection of measured
layout drift, and failure/message metadata.
