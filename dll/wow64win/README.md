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
process connection and selector-dependent calls. `callback.c` creates a
nested CPU frame on the native call stack, packs arguments on the guest
stack, enters i386 ntdll's callback dispatcher, and widens returned data
before NtCallbackReturn. The previous CPU frame and guest exception chain
are restored on ordinary callback return. Creation strings are packed
relative to their translated CREATESTRUCT, matching ReactOS user32.

Current limits include hooks, DDE, IME callbacks, most private structures,
printer driver pointers, and unconverted pointer messages. Cross-callback
RtlUnwind explicitly terminates with STATUS_NOT_SUPPORTED; resuming an
incorrect outer CPU frame is not permitted. Callback allocation rejects
stack exhaustion below the currently committed guest stack. Arbitrary
application-defined messages retain their normal integer transport; the
application must define its own cross-bitness protocol.

The explicit `wow64_first_window` and `wow64_win32k_abi` targets are excluded
from normal images. Run the same PE32 executables on i386 and WoW64. The GUI
fixture requires an external mouse click and checks creation payloads,
painting, button notification, destruction and class unregistration.
