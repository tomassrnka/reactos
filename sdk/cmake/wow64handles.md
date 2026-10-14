The per-type table implements the requested phase-3b revision a48e73f1243:
USER/NT sign-extend; GDI/HKL/address handles zero-extend; all narrowings truncate.
**The HKL zero rule does not match either Windows reference.** It remains an
explicit ReactOS WoW64 policy in this table, pending a phase-3c design decision.

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

Both references issue these bit-31 handles sign-extended. Zero extension still
works for the tested USER and GDI operations. For HKL, zero extension fails in
all three measured paths. The fixture therefore reports exactly four failures
out of 34 checks on each Windows reference: the assertions for accepting the
zero-extended HKL. Do not interpret this as a passing Windows conformance test.
Other USER/GDI handle classes and address-valued/NT handles were not measured.
Typed SelectObject/PatBlt with the zero-extended brush also renders the expected
pixel on both references; the signed SelectObject variant was not tested.

ReactOS also has an issuance discrepancy with the design rationale:
`NtUserLoadKeyboardLayoutEx` calls `UlongToHandle(dwNewKL)`, but the amd64
`ULongToHandle` helper in `sdk/include/psdk/basetsd.h` takes a **signed** `long`.
The baseline consequently issues Dvorak as `FFFFFFFFF0020409`, and rejects the
zero form. This change leaves that global helper and native behavior intact.
The WoW64-enabled HKL comparisons accept both forms, making the requested table
usable with this kernel, but broader than the measured Windows behavior.

On ReactOS amd64, the same final fixture goes from five failures before the
fix to zero after it: signed bitmap deletion plus four zero-HKL assertions.
Native i386 passes all 34 checks; default native amd64 retains its five baseline
failures. `win32u_apitest NtGdiDeleteObjectApp` goes from 12 failures to six:
the six upper-bit ownership failures disappear, while unrelated error/version
expectations are unchanged. Reuse/unique extraction assertions also pass.

The phase-2 regression comparison uses the same freshly built runtime and
fixtures, changing only win32k between baseline and fixed images. All 56 test
groups (6,819 assertions) have identical results. The single existing failure
is `NtQueryKey.c:88` under WoW64 (`SubKeys = 1`), also present in the recorded
phase-2 baseline. There are no new failures.

Both native architectures and the enabled amd64 build compile with GCC/RosBE,
Debug, export level 0x601. Rebuilt native executable code, data, imports, unwind
and relocations are identical. Whole user32/imm32 files are identical. Relinking
win32k/gdi32 changes export timestamps; amd64 win32k additionally changes six
DWARF source-column bytes, with identical decoded line/address rows. Thus raw
byte identity of every Debug PE file is not claimed.

The brush-result test explicitly synthesizes the integer extension in a native
window procedure. Its drawing already worked on the baseline; it is not a
claimed before/after failure. Actual WoW64 window-procedure dispatch and wide
guest DC_ATTR writes require phase-3c callbacks/thunks and the other phase-3b
lanes. Those integration gates remain open. The ownership and HKL tests above
exercise the repaired kernel paths today. No current production code uses the
reuse-count macro; its upper-bit contract is covered directly by the test.

Reproduce the metadata checks with
`cmake -P sdk/cmake/tests/wow64handles.cmake`, or the existing
`wow64_syscall_tables_test` build target. Changing the HDC row to SIGN makes
the metadata test fail. Build and run `user32_apitest Handle32` on both
architectures and `win32u_apitest NtGdiDeleteObjectApp` on amd64, with and without
`BUILD_WOW64_ENABLED`. Keep the test executable identical for before/after runs.
The amd64 Handle32 fixture used on both references and the final ReactOS runs
has SHA-256 `06bf7a98dd140afb74667f53c2dc6c1ffe1075734c237c189cc02bc925b86958`.
