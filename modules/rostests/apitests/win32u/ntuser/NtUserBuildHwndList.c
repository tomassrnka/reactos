/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for NtUserBuildHwndList with a bad count pointer
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "../win32nt.h"

/* Never mapped */
#define BAD_USER_ADDRESS ((PVOID)(ULONG_PTR)8)
/* Never user memory: the last page of the address space. It is usually
   unmapped, so this case alone would also pass with no probe */
#define KERNEL_ADDRESS ((PVOID)(ULONG_PTR)-4096)
/* Mapped, writable kernel memory: the kernel view of the shared user data
   page, at Reserved2[0] of the NT 5.x/6.x layout, which nothing writes. Only
   a probe keeps the count from being written there. Under WOW64 it is an
   unmapped user address, which fails too */
#ifdef _WIN64
#define SHARED_DATA_KERNEL 0xFFFFF78000000000ULL
#else
#define SHARED_DATA_KERNEL 0xFFDF0000UL
#endif
#define SHARED_DATA_UNUSED_OFFSET 0x248

START_TEST(NtUserBuildHwndList)
{
    NTSTATUS Status;
    ULONG Count;
    HWND List[4];
    ULONG Before;
    volatile ULONG *SharedField = (volatile ULONG *)(ULONG_PTR)(MM_SHARED_USER_DATA_VA + SHARED_DATA_UNUSED_OFFSET);

    /* This prototype is the NT 5.x and 6.0 one */
    if (GetNTVersion() > _WIN32_WINNT_VISTA)
    {
        skip("NtUserBuildHwndList has another prototype on this version\n");
        return;
    }

    /* Desktop windows, valid count pointer: the count is written. Whether
       a zero-sized list is an error is not what this test is about */
    Count = 0xdeadbeef;
    Status = NtUserBuildHwndList(NULL, NULL, FALSE, 0, 0, NULL, &Count);
    ok(Status == STATUS_SUCCESS || Status == STATUS_BUFFER_TOO_SMALL, "Status = 0x%lx\n", Status);
    ok(Count != 0xdeadbeef, "Count was not written\n");

    /* Desktop windows, bad count pointer */
    Status = NtUserBuildHwndList(NULL, NULL, FALSE, 0, 0, NULL, (ULONG *)BAD_USER_ADDRESS);
    ok_hex(Status, STATUS_ACCESS_VIOLATION);
    Status = NtUserBuildHwndList(NULL, NULL, FALSE, 0, _countof(List), List, (ULONG *)KERNEL_ADDRESS);
    ok_hex(Status, STATUS_ACCESS_VIOLATION);
    Before = *SharedField;
    Status = NtUserBuildHwndList(NULL, NULL, FALSE, 0, _countof(List), List,
                                 (ULONG *)(ULONG_PTR)(SHARED_DATA_KERNEL + SHARED_DATA_UNUSED_OFFSET));
    ok_hex(Status, STATUS_ACCESS_VIOLATION);
    ok(*SharedField == Before, "Kernel memory was written: 0x%lx -> 0x%lx\n", Before, *SharedField);

    /* Thread windows, bad count pointer */
    Status = NtUserBuildHwndList(NULL, NULL, FALSE, GetCurrentThreadId(), 0, NULL, (ULONG *)BAD_USER_ADDRESS);
    ok_hex(Status, STATUS_ACCESS_VIOLATION);
}
