The per-type table implements the corrected phase-3b handle rule:
USER/NT/HKL sign-extend; GDI/address handles zero-extend; all narrowings truncate.
HKL matches both the ReactOS issuer and the Windows reference measurements.
GDI ZERO is specific to ReactOS's current win32k, rather than Windows issuance.

`user32_apitest.exe Handle32` ran as a 64-bit process on Windows Server 2008 R2
6.1.7601 and Windows 10 10.0.19045.2006. These are public-API, black-box results.
The test reused owned menus, brushes and bitmaps until bit 31 was set, and loaded
US Dvorak (`00010409`). The two forms always had identical low 32 bits. No
Windows binary was read, disassembled or copied from a reference guest.

| Handle / calls | Server 2008 R2 zero / sign | Windows 10 zero / sign |
|---|---|---|
| HMENU: IsMenu, GetMenuItemCount | accepted / accepted | accepted / accepted |
| HBRUSH: GetObjectW, WM_CTLCOLORSTATIC result followed by FillRect | accepted / accepted | accepted / accepted |
| HBITMAP: signed DeleteObject, followed by GetObjectW on both stale forms | deleted; both stale forms rejected | deleted; both stale forms rejected |
| HKL: MapVirtualKeyExW('Q', MAPVK_VK_TO_VSC) | 0 / 45 | 0 / 45 |
| HKL: ActivateKeyboardLayout | NULL, unchanged / success | NULL, unchanged / success |
| HKL: WM_INPUTLANGCHANGEREQUEST | unchanged / activates Dvorak | unchanged / activates Dvorak |

Representative issued handles were:

| Reference | HMENU | HBRUSH | HBITMAP | HKL |
|---|---|---|---|---|
| Server 2008 R2 | FFFFFFFF80000101 | FFFFFFFF80100164 | FFFFFFFF80050161 | FFFFFFFFF0020409 |
| Windows 10 | FFFFFFFF800001D7 | FFFFFFFFA71008A6 | FFFFFFFF960507C9 | FFFFFFFFF0020409 |

Both references issued the examples above sign-extended. A later Windows 10
run also returned a zero-extended bit-31 brush; GDI issuance can vary by path.
Zero extension still
works for the tested USER and GDI operations. For HKL, zero extension fails in
all three measured paths. Handle32 asserts that rejection on amd64, signed
issuance, and acceptance of the signed form. On i386 the forms are identical,
so both must succeed.
Other USER/GDI handle classes and address-valued/NT handles were not measured.
Typed SelectObject/PatBlt with the zero-extended brush also renders the expected
pixel on both references; the signed SelectObject variant was not tested.

ReactOS issues HKLs through `NtUserLoadKeyboardLayoutEx` calling
`UlongToHandle(dwNewKL)`. The amd64 `ULongToHandle` helper in
`sdk/include/psdk/basetsd.h` takes a **signed** `long`, so Dvorak is issued as
`FFFFFFFFF0020409`. All images retain full-width HKL comparisons. Typed HKL
arguments and HKLs carried in LPARAM both sign-extend; no tolerant comparison
is needed, and accepting the zero form would diverge from Windows.

ReactOS issues bit-31 GDI handles zero-extended and compares DC_ATTR handles
at full width. The GDI table row and the guest DC_ATTR/batch `KSET` stores must
change together if issuance is ever aligned with Windows. Enabled amd64
images tolerate upper bits in the GDI owner check. Default amd64 retains its
existing signed-bitmap deletion failure, marked `todo_ros` in Handle32 only
when that fixture is built without `BUILD_WOW64_ENABLED`. The enabled fixture
requires deletion to succeed. Native GDI compatibility is a separate fix;
`win32u_apitest NtGdiDeleteObjectApp` already covers the upper-half owner cases.

Each Handle32 recycling search is capped at five seconds (three searches,
fifteen seconds total). A failure to obtain an owned bit-31 handle is reported
as an inconclusive skip; the test never invents a handle or borrows one.

The corrected fixture passes 40 checks with no failures or skips on both
Windows references, enabled amd64 ReactOS and native i386. Default amd64
ReactOS passes 41 checks with one expected todo and no failures or skips (the
extra check cleans up after the known signed-bitmap deletion failure). Against
the reviewed enabled kernel, the corrected fixture fails exactly four HKL
checks; restoring full-width equality makes those four pass.

The phase-2 regression comparison uses the same freshly built runtime and
fixtures, changing only win32k between baseline and fixed images. All 56 test
groups (6,819 assertions) have identical results. The single existing failure
is `NtQueryKey.c:88` under WoW64 (`SubKeys = 1`), also present in the recorded
phase-2 baseline. There are no new failures.

Both native architectures and the enabled amd64 build compile with GCC/RosBE,
Debug, export level 0x601. Rebuilt native executable code, data, imports, unwind
and relocations are identical. Relinking changes export timestamps; amd64
win32k also has debug-metadata differences. Thus raw
byte identity of every Debug PE file is not claimed.

The brush-result test explicitly synthesizes the integer extension in a native
window procedure. Its drawing already worked on the baseline; it is not a
claimed before/after failure. Actual WoW64 window-procedure dispatch and wide
guest DC_ATTR writes require phase-3c callbacks/thunks and the other phase-3b
lanes. Those integration gates remain open. The ownership test exercises the
gated GDI repair; the HKL tests require Windows-compatible rejection in both
default and enabled images. No current production code uses the reuse-count
macro; its upper-bit contract is covered directly by the test.

Reproduce the metadata checks with
`cmake -P sdk/cmake/tests/wow64handles.cmake`, or the existing
`wow64_syscall_tables_test` build target. Changing HKL back to ZERO or HDC
to SIGN makes the metadata test fail. Build and run `user32_apitest Handle32`
on both architectures and `win32u_apitest NtGdiDeleteObjectApp` on amd64, with
and without `BUILD_WOW64_ENABLED`. Use the enabled fixture for the enabled
image so its signed-bitmap assertion is mandatory; keep each executable
identical across its before/after pair. The default amd64 fixture also runs
on both Windows references with no ReactOS todo suppression.
