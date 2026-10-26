# Host-native handle-table test harness (T43 spike)

Compiles the real `ntoskrnl/ex/handle.c` with a host gcc against `shim/ntoskrnl.h`
(a minimal copy of the types/primitives it uses) and stress-tests it with pthreads
(one thread == one CPU). Runs in seconds, no VM:

    make -C tools/hosttest/handle run SECS=2
    make -C tools/hosttest/handle selftest-mutation   # proves the harness detects an ABA regression

Checks: no handle given to two owners at once, Map/Unlock/Destroy round-trip,
HandleCount back to 0, full free-list audit (FirstFree + LastFree + per-CPU caches:
no dup/loss/loop), 3-level growth, pool-failure no-leak; shared-list, StrictFIFO and
per-CPU-cache (SMP lever #1) paths, incl. CPU ids beyond the table cpu count.

Limits: cannot see IRQL/DPC reentrancy or cache-line effects (the shim makes
KeRaiseIrqlToDpcLevel a no-op); x86_64 Linux only. `shim/ntoskrnl.h` copies the
HANDLE_TABLE/EXHANDLE layouts from sdk/include/ndk/extypes.h and
ntoskrnl/include/internal/ex.h - update it if those change. Not part of the
ReactOS CMake build.
