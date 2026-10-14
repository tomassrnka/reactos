# Wide guest state fixture

Build the explicit, uninstalled i386 GCC target `wow64_guest_state` and run
`wow64_guest_state.exe GuestState` on ReactOS amd64 with WoW64 enabled and
the per-process client procedure table change. Native i386 and other operating
systems are skipped before any native-mode bridge is entered.

The executable compiles the real user32 readers from `misc.c`, imm32's
`Imm32CurrentPti`, and gdi32's initialization, batch allocator, handle-cache
consumer and `UpdateColors`. Unused functions are removed at link time.
It imports only kernel32, ntdll and msvcrt. The test-only bridge from
`ClientPfn.S` supplies native win32k calls and the thread-startup callback;
it does not initialize or load a guest user32 DLL.

The kernel initializes the process/thread state. An independent long-mode
read of `GS:[0x30]` supplies the native TEB address. Checks cover:

- CLIENTINFO addresses and the kernel's thread flags, expected version,
  keyboard layout, code page, desktop pointer/delta, client-thread pointer
  and full process identity, through the user32/imm32 guest view.
- Win32ThreadInfo reads in the presence of a deliberately stale guest TEB
  value; the opaque-return comparisons here check their low halves. Step 3
  separately converts the full nonpaged-pool identity comparisons.
- DESKTOPINFO and the existing desktop WND through user32's readers, including
  the handle-table path and rectangle agreement with native GetWindowPlacement.
- Guest A/W procedure lookups and the server-to-client W-table prefix after
  registration. The separate `ClientPfn` fixture checks kernel readback,
  process isolation and unchanged native classes.
- Native PEB GDI mappings, device capabilities, batch limit and cache address;
  the inherited wide FLOAT conversion; native batch allocation, DC matching,
  zero extension, thread reset and kernel consumption; full-width cSpins reset.
- Eight-byte native cache slots, using a synthetic private GDI table entry
  and a bit-31 brush handle. The kernel's table is never modified and the
  synthetic handle is never passed to a syscall.

Only language-pack loading and the final UpdateColors syscall are stubbed.
All touched guest/native scratch fields are restored. The cache subtest uses
the actual native PEB buffer but seeds its contents explicitly: this base's
normal brush creation can leave its handle entry's UserData null.

For the failing control, compile these same test sources and CMake target
against the pre-step-4 production sources and headers. `GuestState.h` supplies
only the explicit native layout for the oracle when those old headers do not
declare it. Its production accessors are unchanged. Pointer-dependent tests
stop after a failed address check, allowing the remaining tests to report
without dereferencing the wrong desktop or handle table.

This is a guest-side unit test under WoW64, not a claim of normal GUI DLL
loading, guest-owned window creation or guest callback dispatch. Those require
the phase-3c runtime and the remaining step-3 stores/pointer comparisons. Keep
`wow64_gui_load` as a separate integration check and report its actual result.
