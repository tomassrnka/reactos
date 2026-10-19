/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Kernel-Mode Test Suite special pool test user-mode part
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <kmt_test.h>
#include "MmSpecialPool.h"

/*
 * User mode may not touch kernel addresses: a read or write of a live special-pool allocation,
 * its guard page or a freed one must fail with an access violation, and must never stop the
 * system the way the same access from kernel mode does (0xCC, 0xCD). Needs the special pool
 * configuration of MmSpecialPool.c; skips without it.
 */
static
VOID
TryAccess(
    _In_ ULONGLONG Address,
    _In_ PCSTR What)
{
    volatile UCHAR *Pointer = (volatile UCHAR *)(ULONG_PTR)Address;

    KmtStartSeh()
        (void)*Pointer;
    KmtEndSeh(STATUS_ACCESS_VIOLATION);
    trace("read of the %s at %p: access violation\n", What, (PVOID)Pointer);

    KmtStartSeh()
        *Pointer = 0xF1;
    KmtEndSeh(STATUS_ACCESS_VIOLATION);
}

START_TEST(MmSpecialPoolUser)
{
    SPECIAL_POOL_ADDRESSES Addresses;
    DWORD Error, Length;

    Error = KmtLoadAndOpenDriver(L"MmSpecialPool", FALSE);
    ok_eq_int(Error, ERROR_SUCCESS);
    if (Error)
        return;

    RtlZeroMemory(&Addresses, sizeof(Addresses));
    Length = sizeof(Addresses);
    Error = KmtSendBufferToDriver(IOCTL_GET_ADDRESSES, &Addresses, sizeof(Addresses), &Length);
    ok_eq_int(Error, ERROR_SUCCESS);
    if (Error || !Addresses.Special)
    {
        skip(FALSE, "Special pool does not serve the test tag (see MmSpecialPool.c for the configuration)\n");
        KmtCloseDriver();
        KmtUnloadDriver();
        return;
    }

    TryAccess(Addresses.Live, "live allocation");
    TryAccess(Addresses.Guard, "guard page");
    TryAccess(Addresses.Freed, "freed allocation");

    KmtCloseDriver();
    KmtUnloadDriver();
}
